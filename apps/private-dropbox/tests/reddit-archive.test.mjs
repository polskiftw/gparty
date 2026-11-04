import test from "node:test";
import assert from "node:assert/strict";
import { ArchiveError, parseRedditId, permittedMedia, planMedia, planPost, proxyMedia } from "../src/reddit-archive.js";
import { tarHeader, downloadPost } from "../src/archive-client.js";
import { Zstd } from "@hpcc-js/wasm-zstd";

test("extracts normal, gallery, and redd.it post IDs; rejects unrelated hosts", () => {
  assert.equal(parseRedditId("https://www.reddit.com/r/cats/comments/abcdef/a_post/?utm_source=share"), "abcdef");
  assert.equal(parseRedditId("https://old.reddit.com/comments/ABC123/"), "abc123");
  assert.equal(parseRedditId("https://www.reddit.com/gallery/abc123"), "abc123");
  assert.equal(parseRedditId("https://redd.it/abc123"), "abc123");
  assert.throws(() => parseRedditId("https://www.reddit.com.evil.test/comments/abc123"), ArchiveError);
  assert.throws(() => parseRedditId("http://www.reddit.com/comments/abc123"), ArchiveError);
  assert.throws(() => parseRedditId("https://www.reddit.com/r/cats"), ArchiveError);
});
test("media URL allowlist rejects SSRF, private networks, user info and open redirects", () => {
  assert.equal(permittedMedia("https://i.redd.it/abc123.jpg").hostname, "i.redd.it");
  assert.equal(permittedMedia("https://preview.redd.it/x.png?width=640&amp;height=480").searchParams.get("height"), "480");
  for (const url of [
    "http://i.redd.it/image.jpg", "https://127.0.0.1/private", "https://169.254.169.254/latest",
    "https://i.redd.it.evil.test/image.jpg", "https://i.redd.it:8443/x.jpg",
    "https://a@i.redd.it/x.jpg", "file:///etc/passwd",
  ]) assert.throws(() => permittedMedia(url), ArchiveError, url);
});
test("gallery retrieves every image in display order", () => {
  const planned = planMedia({
    is_gallery: true,
    gallery_data: { items: [{ media_id: "one" }, { media_id: "two" }] },
    media_metadata: {
      one: { m: "image/jpeg", s: { u: "https://i.redd.it/one.jpg" } },
      two: { m: "image/png", s: { u: "https://preview.redd.it/two.png?width=1024" } },
    },
  });
  assert.deepEqual(planned.map((media) => media.name), ["media/001.jpg", "media/002.png"]);
  assert.deepEqual(planned.map((media) => media.kind), ["gallery", "gallery"]);
});
test("video manifests and audio track alternatives are included for native video", () => {
  const planned = planMedia({
    secure_media: { reddit_video: {
      fallback_url: "https://v.redd.it/vid123/DASH_720.mp4?source=fallback",
      dash_url: "https://v.redd.it/vid123/DASHPlaylist.mpd",
    } },
  });
  assert.deepEqual(planned.map((media) => media.kind), ["video", "video-manifest", "video-audio"]);
  assert.ok(planned[2].alternateUrls.length >= 3);
  assert.equal(planned[2].optional, true);
});
test("post plan fetches public post JSON and retains original data", async () => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async (url, options) => {
    assert.match(url, /www\.reddit\.com\/comments\/abc123\.json\?/);
    assert.equal(options.redirect, "manual");
    return Response.json([{ data: { children: [{ kind: "t3", data: {
      id: "abc123", title: "Hello! Example", author: "example", subreddit: "test",
      permalink: "/r/test/comments/abc123/example/", selftext: "**Body**", created_utc: 123456789,
      is_gallery: false, url: "https://i.redd.it/pic.jpg",
    } }] } }]);
  };
  try {
    const result = await planPost("https://redd.it/abc123");
    assert.equal(result.filename, "reddit-abc123-Hello-Example.tar.zst");
    assert.equal(result.folder, "reddit-abc123");
    assert.equal(result.post.selftext, "**Body**");
    assert.equal(result.files[0].name, "media/001.jpg");
    assert.equal(result.post.originalData.id, "abc123");
  } finally { globalThis.fetch = originalFetch; }
});
test("proxy redirects only to approved media origins and streams bytes", async () => {
  const saved = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url) => {
    calls.push(url);
    if (calls.length === 1) return new Response(null, { status: 302, headers: { location: "https://preview.redd.it/next.jpg" } });
    return new Response(Uint8Array.from([1, 2, 3]), { headers: { "content-type": "image/jpeg", "content-length": "3" } });
  };
  try {
    const data = await proxyMedia("https://i.redd.it/first.jpg");
    assert.equal(data.status, 200);
    assert.deepEqual([...new Uint8Array(await data.arrayBuffer())], [1, 2, 3]);
    assert.deepEqual(calls, ["https://i.redd.it/first.jpg", "https://preview.redd.it/next.jpg"]);
    assert.equal(data.headers.get("content-length"), "3");
  } finally { globalThis.fetch = saved; }
});
test("rejects CDN redirects to arbitrary websites", async () => {
  const saved = globalThis.fetch;
  globalThis.fetch = async () => new Response(null, { status: 302, headers: { location: "https://localhost/secret" } });
  try { await assert.rejects(() => proxyMedia("https://v.redd.it/test/DASH_720.mp4"), ArchiveError); }
  finally { globalThis.fetch = saved; }
});
test("TAR header uses USTAR format and correct octal checksum", () => {
  const block = tarHeader("reddit-abc123/media/001.jpg", 1000, 0);
  assert.equal(block.byteLength, 512);
  assert.equal(new TextDecoder().decode(block.slice(257, 262)), "ustar");
  const readOctal = (start, length) => parseInt(new TextDecoder().decode(block.slice(start, start + length)), 8);
  assert.equal(readOctal(124, 12), 1000);
  const expected = readOctal(148, 8);
  const check = block.slice(); check.fill(32, 148, 156);
  assert.equal(check.reduce((sum, byte) => sum + byte, 0), expected);
  assert.throws(() => tarHeader("../secret", 5));
});

test("end-to-end: generated file is a real zstd-compressed TAR with text and all gallery media", async () => {
  const savedFetch = globalThis.fetch;
  const savedDocument = globalThis.document;
  const savedCreate = URL.createObjectURL;
  const savedRevoke = URL.revokeObjectURL;
  const savedTimeout = globalThis.setTimeout;
  let resultBlob, clicked = "";
  const plan = {
    filename: "reddit-abc123-test.tar.zst", folder: "reddit-abc123",
    post: {
      title: "Test archive", permalink: "https://www.reddit.com/comments/abc123/",
      author: "example", subreddit: "test", createdUtc: 123456789, selftext: "Example **text**",
      archivedAt: "2026-11-04T00:00:00.000Z", originalData: { id: "abc123" },
    },
    files: [
      { name: "media/001.jpg", url: "https://i.redd.it/one.jpg", kind: "gallery" },
      { name: "media/002.png", url: "https://i.redd.it/two.png", kind: "gallery" },
    ],
  };
  try {
    globalThis.fetch = async (path, init) => {
      assert.equal(init.method, "POST");
      assert.equal(init.headers["x-gparty-write"], "1");
      if (path === "/_/api/archive/plan") return Response.json(plan);
      if (path === "/_/api/archive/media") {
        const { url } = JSON.parse(init.body);
        const bytes = url.endsWith("one.jpg") ? Uint8Array.from([1, 2, 3, 4]) : Uint8Array.from([7, 8, 9]);
        return new Response(bytes, { headers: { "content-length": String(bytes.length) } });
      }
      throw new Error("Unexpected URL " + path);
    };
    URL.createObjectURL = (blob) => { resultBlob = blob; return "blob:fake"; };
    URL.revokeObjectURL = () => {};
    globalThis.setTimeout = () => 0;
    globalThis.document = {
      createElement: (tag) => {
        assert.equal(tag, "a");
        return { click() { clicked = this.download; }, remove() {} };
      },
      body: { append() {} },
    };
    const progress = [];
    const result = await downloadPost("https://redd.it/abc123", (message) => progress.push(message));
    assert.equal(result.filename, plan.filename);
    assert.equal(result.completed.length, 2);
    assert.equal(clicked, plan.filename);
    assert.ok(resultBlob instanceof Blob);
    const packed = new Uint8Array(await resultBlob.arrayBuffer());
    assert.deepEqual([...packed.slice(0, 4)], [0x28, 0xb5, 0x2f, 0xfd]);
    const zstd = await Zstd.load();
    const tar = zstd.decompress(packed);
    const items = new Map();
    let offset = 0;
    while (offset + 512 <= tar.length) {
      const header = tar.subarray(offset, offset + 512);
      if (header.every((byte) => byte === 0)) break;
      const name = new TextDecoder().decode(header.subarray(0, 100)).split("\0")[0];
      const size = parseInt(new TextDecoder().decode(header.subarray(124, 136)), 8);
      offset += 512;
      items.set(name, tar.slice(offset, offset + size));
      offset += Math.ceil(size / 512) * 512;
    }
    assert.ok(new TextDecoder().decode(items.get("reddit-abc123/post.md")).includes("Example **text**"));
    assert.equal(JSON.parse(new TextDecoder().decode(items.get("reddit-abc123/post.json"))).originalData.id, "abc123");
    assert.deepEqual([...items.get("reddit-abc123/media/001.jpg")], [1, 2, 3, 4]);
    assert.deepEqual([...items.get("reddit-abc123/media/002.png")], [7, 8, 9]);
    assert.ok(items.has("reddit-abc123/ARCHIVE_NOTES.txt"));
    assert.match(progress.at(-1), /Download ready/);
  } finally {
    globalThis.fetch = savedFetch;
    globalThis.document = savedDocument;
    URL.createObjectURL = savedCreate;
    URL.revokeObjectURL = savedRevoke;
    globalThis.setTimeout = savedTimeout;
  }
});
