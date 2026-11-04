import test from "node:test";
import assert from "node:assert/strict";
import { ArchiveError, parseRedditId, permittedMedia, planMedia, planPost, proxyMedia } from "../src/reddit-archive.js";
import { tarHeader } from "../src/archive-client.js";

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
