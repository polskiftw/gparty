import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const source = readFileSync(new URL("../src/worker.js", import.meta.url), "utf8")
  .replace('import { PAGE_HTML } from "./page.js";', 'const PAGE_HTML = "<html>Gooning.party</html>";')
  .replace('import APP_JS from "./app.js";', 'const APP_JS = "console.log(1)";')
  .replace('import APP_CSS from "./style.css";', 'const APP_CSS = "body{}";')
  .replace('import CHAOS_JS from "./chaos.js";', 'const CHAOS_JS = "console.log(2)";')
  .replace('import CURSOR_SVG from "./cursor.svg";', 'const CURSOR_SVG = "<svg></svg>";')
  .replace('import WEBMASTER_SVG from "./webmaster.svg";', 'const WEBMASTER_SVG = "<svg></svg>";')
  .replace('import ANTHEM_MIDI from "./internet-anthem.mid";', 'const ANTHEM_MIDI = new Uint8Array([77,84,104,100]);')
  .replace('from "./ai-bridge.js";', 'from "' + new URL("../src/ai-bridge.js", import.meta.url).href + '";')
  .replace('from "./multipart.js";', 'from "' + new URL("../src/multipart.js", import.meta.url).href + '";');
const moduleUrl = "data:text/javascript;base64," + Buffer.from(source).toString("base64");
const worker = (await import(moduleUrl)).default;

class FakeR2 {
  constructor() { this.entries = new Map(); this.nextEtag = 1; }
  meta(key, entry) {
    return {
      key, size: entry.bytes.length, etag: entry.etag, httpEtag: '"' + entry.etag + '"',
      uploaded: entry.uploaded, customMetadata: entry.customMetadata,
    };
  }
  async head(key) {
    const entry = this.entries.get(key);
    return entry ? this.meta(key, entry) : null;
  }
  async put(key, body, options = {}) {
    if (options.onlyIf?.get?.("if-none-match") === "*" && this.entries.has(key)) return null;
    const bytes = body instanceof ReadableStream
      ? new Uint8Array(await new Response(body).arrayBuffer())
      : new TextEncoder().encode(String(body));
    const entry = {
      bytes, etag: String(this.nextEtag++), uploaded: new Date(),
      customMetadata: options.customMetadata || {},
    };
    this.entries.set(key, entry);
    return this.meta(key, entry);
  }
  async get(key, options = {}) {
    const entry = this.entries.get(key);
    if (!entry) return null;
    const meta = this.meta(key, entry);
    const selected = options.range
      ? entry.bytes.slice(options.range.offset, options.range.offset + options.range.length)
      : entry.bytes;
    return {
      ...meta, ...(options.range ? { range: { offset: options.range.offset, length: selected.length } } : {}),
      body: new Response(selected).body,
      text: async () => new TextDecoder().decode(selected),
      arrayBuffer: async () => Uint8Array.from(selected).buffer,
    };
  }
  async list({ prefix, delimiter }) {
    const objects = [], delimitedPrefixes = new Set();
    for (const [key, entry] of this.entries) {
      if (!key.startsWith(prefix)) continue;
      const rest = key.slice(prefix.length), slash = rest.indexOf(delimiter);
      if (slash !== -1) delimitedPrefixes.add(prefix + rest.slice(0, slash + 1));
      else objects.push(this.meta(key, entry));
    }
    return { objects, delimitedPrefixes: [...delimitedPrefixes], truncated: false };
  }
}

const cert = { certPresented: "1", certVerified: "SUCCESS", certRevoked: "0" };
function req(path, options = {}, good = true) {
  const request = new Request("https://gooning.party" + path, options);
  if (good) request.cf = { tlsClientAuth: cert };
  return request;
}
function post(path, body, extra = {}) {
  return req(path, {
    method: "POST", body,
    headers: {
      "x-gparty-write": "1", origin: "https://gooning.party",
      "content-length": String(new TextEncoder().encode(body).byteLength), ...extra,
    },
  });
}
function env(bucket) { return { PRIVATE_FILES_BUCKET: bucket }; }
async function payload(response) { return response.json(); }

test("unverified visitors cannot see the root, assets, or APIs", async () => {
  const bucket = new FakeR2();
  for (const path of ["/", "/_/app.js", "/_/chaos.js", "/_/cursor.svg", "/_/webmaster.svg", "/_/internet-anthem.mid", "/_/api/list", "/secret.log"]) {
    assert.equal((await worker.fetch(req(path, {}, false), env(bucket))).status, 403);
  }
  const revoked = req("/");
  revoked.cf.tlsClientAuth = { ...cert, certRevoked: "1" };
  assert.equal((await worker.fetch(revoked, env(bucket))).status, 403);
});
test("folder creation, traversal prevention, direct URLs and safe MIME", async () => {
  const bucket = new FakeR2(), e = env(bucket);
  assert.equal((await worker.fetch(req("/"), e)).status, 200);
  assert.equal((await worker.fetch(req("/_/api/folder", { method: "POST", body: "{}" }), e)).status, 403);
  assert.equal((await worker.fetch(post("/_/api/folder", JSON.stringify({ path: "Valheim" })), e)).status, 201);
  const root = await payload(await worker.fetch(req("/_/api/list?path="), e));
  assert.deepEqual(root.folders, ["Valheim"]);
  assert.equal((await worker.fetch(req("/Valheim/"), e)).status, 200);
  assert.equal((await worker.fetch(post("/_/api/folder", JSON.stringify({ path: "Valheim/.." })), e)).status, 400);
  assert.equal((await worker.fetch(post("/_/api/folder", JSON.stringify({ path: "_/oops" })), e)).status, 400);
  assert.equal((await worker.fetch(post("/_/api/upload?path=Valheim%2FPlayer.log", "ERROR"), e)).status, 201);
  assert.equal((await worker.fetch(post("/_/api/upload?path=Valheim%2FPlayer.log", "ERROR"), e)).status, 409);
  const files = await payload(await worker.fetch(req("/_/api/list?path=Valheim"), e));
  assert.equal(files.files[0].name, "Player.log");
  const raw = await worker.fetch(req("/Valheim/Player.log"), e);
  assert.equal(await raw.text(), "ERROR");
  assert.match(raw.headers.get("content-type"), /^text\/plain/);
  assert.equal((await payload(await worker.fetch(req("/_/api/preview?path=Valheim%2FPlayer.log"), e))).text, "ERROR");
  assert.equal((await worker.fetch(post("/_/api/upload?path=Valheim%2Funsafe.html", "<script>x</script>"), e)).status, 201);
  const html = await worker.fetch(req("/Valheim/unsafe.html"), e);
  assert.equal(html.headers.get("content-type"), "application/octet-stream");
  assert.match(html.headers.get("content-disposition"), /^attachment/);
});
test("same-origin protection and upload-size enforcement", async () => {
  const e = env(new FakeR2());
  assert.equal((await worker.fetch(post("/_/api/folder", '{"path":"Bad"}', { origin: "https://evil.example" }), e)).status, 403);
  assert.equal((await worker.fetch(req("/_/api/upload?path=huge.log", {
    method: "POST", body: "abc",
    headers: { "x-gparty-write": "1", "content-length": String(65 * 1024 * 1024) },
  }), e)).status, 413);
});
test("the GitHub bridge refuses a public repo and atomically commits approved logs", async () => {
  const bucket = new FakeR2(), e = {
    ...env(bucket), AI_GITHUB_REPO: "polskiftw/private-logs-test", AI_GITHUB_TOKEN: "test-only-token",
  };
  assert.equal((await worker.fetch(post("/_/api/upload?path=Player.log", "oops"), e)).status, 201);
  let remotePrivate = false, blobs = 0, commits = 0;
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async (url, init) => {
    assert.equal(init.headers.authorization, "Bearer test-only-token");
    const path = url.split("/repos/polskiftw/private-logs-test")[1];
    if (!path) return Response.json({ private: remotePrivate, archived: false, default_branch: "main" });
    if (path === "/git/blobs") {
      blobs++;
      assert.equal(JSON.parse(init.body).content, btoa("oops"));
      return Response.json({ sha: "b".repeat(40) }, { status: 201 });
    }
    if (path === "/git/ref/heads/main") return Response.json({ object: { sha: "a".repeat(40) } });
    if (path === "/git/commits/" + "a".repeat(40)) return Response.json({ tree: { sha: "b".repeat(40) } });
    if (path === "/git/trees") return Response.json({ sha: "c".repeat(40) }, { status: 201 });
    if (path === "/git/commits") {
      commits++;
      return Response.json({ sha: "d".repeat(40) }, { status: 201 });
    }
    if (path === "/git/refs/heads/main") return Response.json({ object: { sha: "d".repeat(40) } });
    throw new Error("Unexpected GitHub request: " + path);
  };
  try {
    const body = JSON.stringify({ path: "Player.log", part: 0 });
    assert.equal((await worker.fetch(post("/_/api/share", body), e)).status, 422);
    assert.equal(blobs, 0);
    remotePrivate = true;
    const ready = await payload(await worker.fetch(post("/_/api/share", body), e));
    assert.equal(ready.totalParts, 1);
    assert.equal(blobs, 1);
    const finished = await worker.fetch(post("/_/api/share/finish", JSON.stringify({
      path: "Player.log", etag: ready.etag, parts: [ready.blobSha],
    })), e);
    assert.equal(finished.status, 200);
    assert.equal((await payload(finished)).githubPath, "shared/Player.log");
    assert.equal(commits, 1);
    assert.equal((await payload(await worker.fetch(req("/_/api/list"), e))).files[0].ai, true);
  } finally { globalThis.fetch = originalFetch; }
});


test("an R2 folder write failure returns traceable JSON instead of an HTML error", async () => {
  class FailingR2 extends FakeR2 {
    async put(key, body, options) {
      if (key.startsWith("folders/")) throw new Error("Simulated R2 write failure");
      return super.put(key, body, options);
    }
  }
  const response = await worker.fetch(
    post("/_/api/folder", JSON.stringify({ path: "Valheim" })),
    env(new FailingR2()),
  );
  assert.equal(response.status, 503);
  assert.match(response.headers.get("content-type"), /application\/json/);
  assert.equal(response.headers.get("x-gparty-api"), "1");
  const result = await payload(response);
  assert.match(result.error, /Operation temporarily unavailable/);
  assert.match(result.errorId, /^[0-9a-f-]{36}$/i);
});

test("MIDI, artwork and visual script remain certificate-protected local assets", async () => {
  const e = env(new FakeR2());
  for (const [path, type] of [
    ["/_/chaos.js", "text/javascript"],
    ["/_/cursor.svg", "image/svg+xml"],
    ["/_/webmaster.svg", "image/svg+xml"],
    ["/_/internet-anthem.mid", "audio/midi"],
  ]) {
    const response = await worker.fetch(req(path), e);
    assert.equal(response.status, 200, path);
    assert.ok(response.headers.get("content-type").startsWith(type), path);
  }
});
test("the private presence counter rejects cross-origin changes and reports real totals", async () => {
  const bucket = new FakeR2();
  let counterCalls = 0;
  const e = {...env(bucket), VISITOR_COUNTER: {
    idFromName(name) {assert.equal(name, "gparty-sitewide");return "shared-counter";},
    get(id) {assert.equal(id, "shared-counter");return {async fetch(request) {
      counterCalls++;
      const payload = await request.json();
      return Response.json({visits:counterCalls,online:1,...payload},{
        headers:{"x-gparty-api":"1"},
      });
    }};},
  }};
  const valid = JSON.stringify({action:"hello",sessionId:"123e4567-e89b-42d3-a456-426614174000"});
  assert.equal((await worker.fetch(post("/_/api/presence",valid,{origin:"https://evil.example"}),e)).status,403);
  assert.equal((await worker.fetch(post("/_/api/presence",'{"action":"hello","sessionId":"bad"}'),e)).status,400);
  assert.equal((await worker.fetch(post("/_/api/presence",valid),env(bucket))).status,503);
  const response = await worker.fetch(post("/_/api/presence",valid),e);
  assert.equal(response.status,200);
  assert.deepEqual((await response.json()).visits,1);
  assert.equal(counterCalls,1);
  assert.equal((await worker.fetch(post("/_/api/presence",valid),e)).status,200);
  assert.equal(counterCalls,2);
});
