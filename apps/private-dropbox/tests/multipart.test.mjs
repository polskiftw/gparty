import test from "node:test";
import assert from "node:assert/strict";
import {
  partSizeFor, startMultipart, uploadMultipartPart,
  finishMultipart, cancelMultipart, MAX_FILE_BYTES,
} from "../src/multipart.js";

const MiB = 1024 * 1024;
class Bucket {
  constructor() { this.sessions = new Map(); this.parts = new Map(); this.done = false; }
  async head() { return null; }
  async get(key) {
    const body = this.sessions.get(key);
    return body === undefined ? null : { text: async () => body };
  }
  async put(key, value) { this.sessions.set(key, value); }
  async delete(key) { this.sessions.delete(key); }
  async createMultipartUpload(key) {
    this.key = key;
    this.id = "valid-multipart-test-id";
    return this.resumeMultipartUpload(key, this.id);
  }
  resumeMultipartUpload(key, id) {
    assert.equal(key, this.key);
    assert.equal(id, this.id);
    return {
      uploadId: id,
      uploadPart: async (partNumber, stream) => {
        this.parts.set(partNumber, await new Response(stream).arrayBuffer());
        return { partNumber, etag: partNumber.toString(16).padStart(32, "0") };
      },
      complete: async (parts) => {
        assert.equal(parts.length, 5);
        this.done = true;
        return { size: 65 * MiB };
      },
      abort: async () => { this.aborted = true; },
    };
  }
}
async function asJson(response) { return await response.json(); }

test("R2 multipart capacity is vastly larger than the former 64 MiB cap", () => {
  assert.equal(partSizeFor(65 * MiB), 16 * MiB);
  assert.equal(partSizeFor(300 * 1024 ** 3), 40 * MiB);
  assert.throws(() => partSizeFor(MAX_FILE_BYTES + 1));
});

test("Multipart parts are authenticated upstream and their lengths checked", async () => {
  const bucket = new Bucket();
  const state = await asJson(await startMultipart(bucket, "large.zip", 65 * MiB));
  assert.equal(state.partSize, 16 * MiB);
  assert.equal(state.partCount, 5);
  assert.equal(bucket.key, "files/large.zip");
  const parts = [];
  for (let i = 1; i <= state.partCount; i++) {
    const size = i === state.partCount ? 1 * MiB : 16 * MiB;
    const body = new Uint8Array(size);
    const request = { headers: new Headers({ "content-length": String(size) }), body: new Response(body).body };
    const response = await uploadMultipartPart(bucket, request, "large.zip", state.uploadId, i);
    assert.equal(response.status, 200);
    parts.push(await asJson(response));
  }
  const result = await finishMultipart(bucket, "large.zip", state.uploadId, parts);
  assert.equal(result.status, 200);
  assert.equal((await asJson(result)).size, 65 * MiB);
  assert.equal(bucket.done, true);
});

test("Rejects tampered part numbers and supports cancellation", async () => {
  const bucket = new Bucket();
  const state = await asJson(await startMultipart(bucket, "large.bin", 65 * MiB));
  const invalid = await uploadMultipartPart(bucket, {
    headers: new Headers({ "content-length": "10" }), body: new Response("no").body,
  }, "large.bin", state.uploadId, 300);
  assert.equal(invalid.status, 400);
  const cancelled = await cancelMultipart(bucket, "large.bin", state.uploadId);
  assert.equal(cancelled.status, 200);
  assert.equal(bucket.aborted, true);
});
