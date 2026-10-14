import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";

const frontend = readFileSync(new URL("../src/app.js", import.meta.url), "utf8");
const start = frontend.indexOf("async function requestJson(");
const finish = frontend.indexOf("\nconst writeHeaders =", start);
assert.ok(start > 0 && finish > start, "frontend requestJson function must exist");
const functionSource = frontend.slice(start, finish);
async function invoke(responseOrThrow, method = "POST") {
  const errors = [];
  const sandbox = {
    fetch: async () => {
      if (responseOrThrow instanceof Error) throw responseOrThrow;
      return responseOrThrow;
    },
    console: { error: (...items) => errors.push(items) },
  };
  vm.createContext(sandbox);
  vm.runInContext(functionSource, sandbox);
  const action = vm.runInContext(
    'requestJson("/_/api/folder", { method: ' + JSON.stringify(method) + ' })',
    sandbox,
  );
  return { action, errors };
}

test("shows HTTP status and Cloudflare Ray when edge returns an HTML 403", async () => {
  const page = '<!doctype html><html><head><title>Attention Required! | Cloudflare</title></head>'
    + '<body>Block</body></html>';
  const response = new Response(page, {
    status: 403,
    headers: { "content-type": "text/html", "cf-ray": "test-ray-123" },
  });
  const { action, errors } = await invoke(response);
  await assert.rejects(action, (cause) => {
    assert.match(cause.message, /POST \/_\/api\/folder returned HTTP 403/i);
    assert.match(cause.message, /Ray ID: test-ray-123/);
    assert.match(cause.message, /Cloudflare Security > Events/);
    assert.doesNotMatch(cause.message, /JSON.parse/);
    return true;
  });
  assert.equal(errors.length, 1);
});

test("exposes Worker error 1101 instead of generic JSON parse error", async () => {
  const response = new Response("<html><title>Worker error</title>Error 1101</html>", {
    status: 500, headers: { "content-type": "text/html" },
  });
  const { action } = await invoke(response);
  await assert.rejects(action, /Worker runtime error/);
});

test("preserves API JSON 503 error IDs", async () => {
  const response = Response.json({
    error: "Operation temporarily unavailable. Check Cloudflare Worker logs.",
    errorId: "test-event-id",
  }, { status: 503, headers: { "cf-ray": "ray-456", "x-gparty-api": "1" } });
  const { action } = await invoke(response);
  await assert.rejects(action, /Worker error ID: test-event-id.*Ray ID: ray-456/);
});

test("accepts valid JSON results for successful folder creation", async () => {
  const response = Response.json({ created: true, path: "Valheim" }, { status: 201 });
  const { action } = await invoke(response);
  assert.equal((await action).path, "Valheim");
});

test("shows network failures without JSON parser exceptions", async () => {
  const { action } = await invoke(new TypeError("Failed to fetch"));
  await assert.rejects(action, /network error \(Failed to fetch\)/);
});
