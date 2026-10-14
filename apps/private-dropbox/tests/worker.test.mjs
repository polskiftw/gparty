import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const source = readFileSync(new URL("../src/worker.js", import.meta.url), "utf8")
  .replace('import { PAGE_HTML } from "./page.js";', 'const PAGE_HTML = "<html>Gooning.party</html>";')
  .replace('import APP_JS from "./app.js";', 'const APP_JS = "console.log(1)";')
  .replace('import APP_CSS from "./style.css";', 'const APP_CSS = "body{}";')
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
  for (const path of ["/", "/_/app.js", "/_/api/list", "/secret.log"]) {
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
test("the GitHub bridge refuses a public repo and syncs only with explicit approval", async () => {
  const bucket = new FakeR2(), e = {
    ...env(bucket), AI_GITHUB_REPO: "polskiftw/private-logs-test", AI_GITHUB_TOKEN: "test-only-token",
  };
  assert.equal((await worker.fetch(post("/_/api/upload?path=Player.log", "oops"), e)).status, 201);
  let remotePrivate = false, writes = 0;
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async (url, init) => {
    assert.equal(init.headers.authorization, "Bearer test-only-token");
    if (url.endsWith("/polskiftw/private-logs-test")) {
      return Response.json({ private: remotePrivate, archived: false });
    }
    if (init.method === "PUT") {
      writes++;
      assert.equal(JSON.parse(init.body).content, btoa("oops"));
      return Response.json({ content: { html_url: "https://github.com/polskiftw/private-logs-test/blob/main/shared/Player.log" } }, { status: 201 });
    }
    return Response.json({ message: "Not found" }, { status: 404 });
  };
  try {
    const body = JSON.stringify({ path: "Player.log" });
    assert.equal((await worker.fetch(post("/_/api/share", body), e)).status, 403);
    assert.equal(writes, 0);
    remotePrivate = true;
    const shared = await payload(await worker.fetch(post("/_/api/share", body), e));
    assert.equal(shared.shared, true);
    assert.equal(shared.githubPath, "shared/Player.log");
    assert.equal(writes, 1);
    const updated = await payload(await worker.fetch(req("/_/api/list"), e));
    assert.equal(updated.files[0].ai, true);
  } finally { globalThis.fetch = originalFetch; }
});
