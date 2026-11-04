import test from "node:test";
import assert from "node:assert/strict";
import { submitArchiveJob, archiveJobStatus, downloadArchive, validateUrl } from "../src/archive-jobs.js";

class FakeBucket {
  entries = new Map();
  async put(key, value) {
    this.entries.set(key, value instanceof Uint8Array ? value : typeof value === "string" ? value : JSON.stringify(value));
  }
  async get(key) {
    const entry = this.entries.get(key);
    if (entry == null) return null;
    const bytes = typeof entry === "string" ? new TextEncoder().encode(entry) : entry;
    return { body: new Response(bytes).body, size: bytes.length,
      async json() { return JSON.parse(new TextDecoder().decode(bytes)); } };
  }
  async delete(key) { this.entries.delete(key); }
}

test("accepts Reddit post links but rejects arbitrary hosts and nonposts", () => {
  assert.equal(new URL(validateUrl("https://www.reddit.com/r/test/comments/abc123/title/?utm_source=share")).search, "");
  assert.match(validateUrl("https://redd.it/abc123"), /redd.it/);
  for (const url of [
    "http://www.reddit.com/r/test/comments/abc123/",
    "https://127.0.0.1/", "https://reddit.com.evil.com/r/test/comments/abc123/",
    "https://www.reddit.com/r/test/", "https://www.reddit.com/user/example/",
    "https://user:pass@reddit.com/comments/abc123/",
  ]) assert.throws(() => validateUrl(url), undefined, url);
});
test("private post URL stays in private R2; only opaque ID is dispatched to GitHub", async () => {
  const bucket = new FakeBucket();
  const env = { PRIVATE_FILES_BUCKET: bucket, ARCHIVE_GITHUB_TOKEN: "private-test-token" };
  const savedFetch = globalThis.fetch;
  let dispatched;
  globalThis.fetch = async (url, init) => {
    assert.match(url, /^https:\/\/api\.github\.com\/repos\/polskiftw\/gparty\/actions\/workflows\/archive-reddit-post\.yml\/dispatches$/);
    assert.equal(init.headers.authorization, "Bearer private-test-token");
    dispatched = JSON.parse(init.body);
    assert.deepEqual(Object.keys(dispatched.inputs), ["job_id"]);
    assert.equal(dispatched.ref, "main");
    return new Response(null, { status: 204 });
  };
  try {
    const url = "https://www.reddit.com/r/private/comments/abc123/my-private-post/";
    const created = await submitArchiveJob({ url }, env);
    assert.equal(created.status, 202);
    const { id } = await created.json();
    assert.equal(dispatched.inputs.job_id, id);
    const stored = JSON.parse(bucket.entries.get("archive-jobs/" + id + "/request.json"));
    assert.equal(stored.url, url);
    const pending = await archiveJobStatus(bucket, id);
    assert.equal((await pending.json()).state, "queued");
    assert.equal((await downloadArchive(bucket, id)).status, 409);
    await bucket.put("archive-jobs/" + id + "/archive.tar.zst", Uint8Array.from([0x28,0xb5,0x2f,0xfd]));
    await bucket.put("archive-jobs/" + id + "/status.json",JSON.stringify({
      state:"ready",filename:"reddit-abc123-test.tar.zst",message:"OK",
    }));
    const ready = await archiveJobStatus(bucket, id);
    assert.equal((await ready.json()).state, "ready");
    const archive = await downloadArchive(bucket, id);
    assert.equal(archive.headers.get("content-type"), "application/zstd");
    assert.match(archive.headers.get("content-disposition"), /reddit-abc123-test.tar.zst/);
    assert.deepEqual([...new Uint8Array(await archive.arrayBuffer())], [0x28,0xb5,0x2f,0xfd]);
  } finally { globalThis.fetch = savedFetch; }
});
test("fails closed when dispatch token is unavailable or GitHub rejects requests", async () => {
  const bucket = new FakeBucket();
  const url = "https://www.reddit.com/comments/abc123/";
  assert.equal((await submitArchiveJob({url}, { PRIVATE_FILES_BUCKET:bucket })).status,503);
  const savedFetch = globalThis.fetch;
  globalThis.fetch = async () => new Response("Forbidden", { status:403 });
  try {
    assert.equal((await submitArchiveJob({url},{PRIVATE_FILES_BUCKET:bucket,ARCHIVE_GITHUB_TOKEN:"bad"})).status,502);
    assert.equal(bucket.entries.size,0);
  } finally { globalThis.fetch = savedFetch; }
});
