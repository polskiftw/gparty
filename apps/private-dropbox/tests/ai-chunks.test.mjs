import test from "node:test";
import assert from "node:assert/strict";
import { shareTextChunk, finishTextShare } from "../src/ai-bridge.js";

const KiB = 1024;
class Bucket {
  constructor(content) {
    this.bytes = new TextEncoder().encode(content);
    this.etag = "source-etag-123";
    this.marker = null;
  }
  async head(key) {
    return key === "files/debug.log" ? { size: this.bytes.length, etag: this.etag } : null;
  }
  async get(key, { range }) {
    assert.equal(key, "files/debug.log");
    const bytes = this.bytes.slice(range.offset, range.offset + range.length);
    return { arrayBuffer: async () => bytes.buffer };
  }
  async put(key, body, options) {
    assert.equal(key, "ai-shares/debug.log");
    this.marker = options.customMetadata.etag;
    return {};
  }
}

test("UTF-8 bridge copies logs over 1 MiB intact in 512 KiB readable parts, one commit", async () => {
  const original = "a".repeat(512 * KiB - 1) + "😀" + "b".repeat(512 * KiB + 7);
  const bucket = new Bucket(original);
  assert.equal(bucket.bytes.length, 1024 * KiB + 10);
  const env = { AI_GITHUB_REPO: "someone/private-logs", AI_GITHUB_TOKEN: "fake-only" };
  const blobs = [], commits = [], trees = [];
  let sequence = 0;
  const savedFetch = globalThis.fetch;
  globalThis.fetch = async (url, options = {}) => {
    const route = url.split("/repos/someone/private-logs")[1];
    if (!route) return Response.json({ private: true, archived: false, default_branch: "main" });
    if (route === "/git/blobs") {
      blobs.push(Buffer.from(JSON.parse(options.body).content, "base64"));
      return Response.json({ sha: (++sequence).toString(16).padStart(40, "0") }, { status: 201 });
    }
    if (route === "/git/ref/heads/main") return Response.json({ object: { sha: "a".repeat(40) } });
    if (route === "/git/commits/" + "a".repeat(40)) return Response.json({ tree: { sha: "b".repeat(40) } });
    if (route === "/git/trees") {
      trees.push(JSON.parse(options.body));
      return Response.json({ sha: "c".repeat(40) }, { status: 201 });
    }
    if (route === "/git/commits") {
      commits.push(JSON.parse(options.body));
      return Response.json({ sha: "d".repeat(40) }, { status: 201 });
    }
    if (route === "/git/refs/heads/main") return Response.json({ object: { sha: "d".repeat(40) } });
    throw new Error("Unexpected GitHub API route: " + route);
  };
  try {
    let etag, receipts = [];
    for (let part = 0; part < 3; part++) {
      const response = await shareTextChunk(bucket, env, "debug.log", part, etag);
      assert.equal(response.status, 200);
      const result = await response.json();
      assert.equal(result.totalParts, 3);
      etag = result.etag;
      receipts.push(result.blobSha);
      assert.equal(bucket.marker, null);
    }
    assert.equal(Buffer.concat(blobs).toString("utf8"), original);
    const final = await finishTextShare(bucket, env, "debug.log", etag, receipts);
    assert.equal(final.status, 200);
    assert.equal((await final.json()).githubPath, "shared/debug.log.index.md");
    assert.equal(commits.length, 1);
    assert.equal(trees.length, 1);
    assert.equal(trees[0].tree.length, 4);
    assert.equal(bucket.marker, etag);
  } finally { globalThis.fetch = savedFetch; }
});

test("GitHub bridge rejects public destinations before uploading anything", async () => {
  const bucket = new Bucket("secret");
  const env = { AI_GITHUB_REPO: "someone/public", AI_GITHUB_TOKEN: "no-real-secret" };
  const savedFetch = globalThis.fetch;
  let calls = 0;
  globalThis.fetch = async () => {
    calls++;
    return Response.json({ private: false, archived: false, default_branch: "main" });
  };
  try {
    const response = await shareTextChunk(bucket, env, "debug.log", 0);
    assert.equal(response.status, 422);
    assert.equal(calls, 1);
    assert.equal(bucket.marker, null);
  } finally { globalThis.fetch = savedFetch; }
});
