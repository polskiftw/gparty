// Bundled on deploy into a same-origin script. Zstandard WASM runs in the browser;
// the Cloudflare Worker supplies Reddit metadata and streams CDN media.
import { Zstd } from "@hpcc-js/wasm-zstd";

const MiB = 1024 * 1024;
const MAX_ITEM = 512 * MiB;
const MAX_TOTAL = 640 * MiB;
const encoder = new TextEncoder();

async function postJson(path, value) {
  const response = await fetch(path, {
    method: "POST", cache: "no-store",
    headers: { "content-type": "application/json", "x-gparty-write": "1" },
    body: JSON.stringify(value),
  });
  if (!response.ok) {
    let message = "HTTP " + response.status;
    try {
      const result = await response.json();
      if (result.error) message += ": " + result.error;
    } catch { /* Cloudflare errors may be plain text or HTML. */ }
    throw new Error(message);
  }
  return response;
}
async function mediaBytes(url, budget) {
  const response = await postJson("/_/api/archive/media", { url });
  const declared = Number(response.headers.get("content-length") || 0);
  if (declared > MAX_ITEM || declared > budget) throw new Error("Media exceeds the archive size limit.");
  if (!response.body) throw new Error("Empty response from media server.");
  const reader = response.body.getReader();
  const chunks = [];
  let length = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      length += value.byteLength;
      if (length > MAX_ITEM || length > budget) {
        await reader.cancel();
        throw new Error("Media exceeds the archive size limit.");
      }
      chunks.push(value);
    }
  } finally { reader.releaseLock(); }
  const bytes = new Uint8Array(length);
  let offset = 0;
  for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.byteLength; }
  return bytes;
}
function writeAscii(block, offset, size, text) {
  const bytes = encoder.encode(String(text));
  if (bytes.length > size) throw new Error("TAR field is too long.");
  block.set(bytes, offset);
}
function writeOctal(block, offset, size, value) {
  const octal = Math.floor(value).toString(8);
  if (octal.length >= size) throw new Error("TAR entry exceeds the supported size.");
  writeAscii(block, offset, size, octal.padStart(size - 1, "0") + "\0");
}
export function tarHeader(name, size, time = Date.now()) {
  if (!/^[a-z0-9_./-]+$/i.test(name) || name.startsWith("/") || name.includes("..") ||
      encoder.encode(name).length > 100 || !Number.isSafeInteger(size) || size < 0) {
    throw new Error("Unsafe TAR file name or size.");
  }
  const block = new Uint8Array(512);
  writeAscii(block, 0, 100, name);
  writeOctal(block, 100, 8, 0o644);
  writeOctal(block, 108, 8, 0);
  writeOctal(block, 116, 8, 0);
  writeOctal(block, 124, 12, size);
  writeOctal(block, 136, 12, Math.floor(time / 1000));
  block.fill(32, 148, 156);
  block[156] = 48; // regular file
  writeAscii(block, 257, 6, "ustar\0");
  writeAscii(block, 263, 2, "00");
  writeAscii(block, 265, 32, "gparty");
  writeAscii(block, 297, 32, "gparty");
  let sum = 0;
  for (const byte of block) sum += byte;
  writeAscii(block, 148, 8, sum.toString(8).padStart(6, "0") + "\0 ");
  return block;
}
function postMarkdown(post) {
  return "# " + post.title + "\n\n"
    + "Original post: " + post.permalink + "\n"
    + "Author: u/" + post.author + "\n"
    + "Subreddit: r/" + post.subreddit + "\n"
    + "Posted: " + new Date(Number(post.createdUtc) * 1000).toISOString() + "\n"
    + (post.crosspostSource ? "Crosspost source: " + post.crosspostSource + "\n" : "")
    + (post.externalUrl ? "Linked URL: " + post.externalUrl + "\n" : "")
    + "\n---\n\n" + (post.selftext || "(No post body text.)") + "\n";
}
function notes(plan, skipped, completed) {
  return [
    "GParty Reddit post archive", "",
    "Post: " + plan.post.permalink,
    "Saved: " + plan.post.archivedAt,
    "Post text: post.md",
    "Structured post metadata: post.json (includes original Reddit post response)",
    "Downloaded media: media/ (" + completed.length + " file(s))",
    "",
    "Native Reddit videos are often served as separate video and audio streams.",
    "Both streams are archived when available, alongside the DASH manifest.",
    "Separate streams are preserved, not remuxed into one MP4.",
    "Links to media on unsupported external sites are recorded in post.json but not fetched.",
    "Comments are not part of this post archive.",
    "",
    completed.length ? "Saved media:\n" + completed.map((entry) => "  " + entry.name + " <- " + entry.url).join("\n") : "No downloadable Reddit-hosted media found.",
    skipped.length ? "\nUnavailable optional media:\n" + skipped.map((entry) => "  " + entry.name + ": " + entry.reason).join("\n") : "",
    "",
  ].join("\n");
}
function downloadBlob(blob, filename) {
  const link = document.createElement("a");
  const objectUrl = URL.createObjectURL(blob);
  link.href = objectUrl;
  link.download = filename;
  link.hidden = true;
  document.body.append(link);
  link.click();
  link.remove();
  // Delayed revoke helps Firefox and Safari finish handling the download.
  setTimeout(() => URL.revokeObjectURL(objectUrl), 60_000);
}
export async function downloadPost(url, onProgress = () => {}) {
  onProgress("Reading Reddit post...");
  const response = await postJson("/_/api/archive/plan", { url });
  const plan = await response.json();
  if (!plan?.folder?.match(/^reddit-[a-z0-9]+$/) || !Array.isArray(plan.files)) {
    throw new Error("Invalid post manifest returned by GParty.");
  }
  onProgress("Loading Zstandard compressor...");
  const zstd = await Zstd.load();
  zstd.resetCompression(3);
  const compressed = [];
  let inputBytes = 0;
  const feed = (bytes) => {
    for (let i = 0; i < bytes.length; i += MiB) {
      const result = zstd.compressChunk(bytes.subarray(i, i + MiB));
      if (result.length) compressed.push(result.slice());
    }
  };
  const append = (name, bytes) => {
    if (inputBytes + bytes.length > MAX_TOTAL) throw new Error("Archive exceeds the 640 MiB limit.");
    const header = tarHeader(plan.folder + "/" + name, bytes.length);
    feed(header);
    feed(bytes);
    const remainder = bytes.length % 512;
    if (remainder) feed(new Uint8Array(512 - remainder));
    inputBytes += bytes.length;
  };
  append("post.json", encoder.encode(JSON.stringify(plan.post, null, 2) + "\n"));
  append("post.md", encoder.encode(postMarkdown(plan.post)));
  const skipped = [], completed = [];
  for (let i = 0; i < plan.files.length; i++) {
    const media = plan.files[i];
    onProgress("Downloading media " + (i + 1) + "/" + plan.files.length + ": " + media.kind + "...");
    const urls = [media.url, ...(media.alternateUrls || [])];
    let data, problem;
    for (const candidate of urls) {
      try {
        data = await mediaBytes(candidate, MAX_TOTAL - inputBytes);
        media.url = candidate;
        break;
      } catch (error) { problem = error; }
    }
    if (!data) {
      if (!media.optional) throw new Error("Could not download " + media.kind + ": " + (problem?.message || "Unknown error"));
      skipped.push({ name: media.name, reason: problem?.message || "Unavailable" });
      continue;
    }
    append(media.name, data);
    completed.push({ name: media.name, url: media.url });
    data = null;
  }
  append("ARCHIVE_NOTES.txt", encoder.encode(notes(plan, skipped, completed)));
  feed(new Uint8Array(1024)); // two mandatory zero blocks terminate the TAR
  const tail = zstd.compressEnd();
  if (tail.length) compressed.push(tail.slice());
  onProgress("Saving " + plan.filename + "...");
  const archive = new Blob(compressed, { type: "application/zstd" });
  downloadBlob(archive, plan.filename);
  onProgress("Download ready: " + plan.filename + " (" + completed.length + " media file(s)"
    + (skipped.length ? ", " + skipped.length + " optional file(s) unavailable" : "") + ").");
  return { filename: plan.filename, completed, skipped, compressedBytes: archive.size };
}
