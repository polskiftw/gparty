// Explicitly approved UTF-8 text is mirrored to a separately verified-private
// GitHub repository in small chunks the ChatGPT GitHub connector can read.
const CHUNK_BYTES = 512 * 1024;
const SHARES = "ai-shares/";
const PREFIX = "shared/";

function json(value, code = 200) {
  return new Response(JSON.stringify(value), {
    status: code,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "private, no-store" },
  });
}
function problem(message, status) { return json({ error: message }, status); }
function encodePath(path) { return path.split("/").map(encodeURIComponent).join("/"); }
function gitSettings(env) {
  const repo = String(env.AI_GITHUB_REPO || "");
  if (!/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(repo) || !env.AI_GITHUB_TOKEN) {
    throw new Error("The private GitHub bridge is not configured.");
  }
  return repo;
}
async function github(env, repo, suffix, options = {}) {
  return fetch("https://api.github.com/repos/" + repo + suffix, {
    ...options,
    headers: {
      accept: "application/vnd.github+json",
      authorization: "Bearer " + env.AI_GITHUB_TOKEN,
      "x-github-api-version": "2022-11-28",
      "user-agent": "GParty-Private-Debug-Bridge",
      ...(options.body ? { "content-type": "application/json" } : {}),
    },
  });
}
function b64(data) {
  let binary = "";
  for (let i = 0; i < data.length; i += 32768) {
    binary += String.fromCharCode(...data.subarray(i, i + 32768));
  }
  return btoa(binary);
}
async function saveFile(env, repo, remote, raw, name) {
  const target = "/contents/" + remote;
  const current = await github(env, repo, target);
  let sha;
  if (current.ok) {
    const file = await current.json();
    if (file.type !== "file" || typeof file.sha !== "string") {
      return problem("GitHub destination conflicts with a directory.", 409);
    }
    sha = file.sha;
  } else if (current.status !== 404) {
    return problem("Could not inspect GitHub destination (HTTP " + current.status + ").", 502);
  }
  const written = await github(env, repo, target, {
    method: "PUT",
    body: JSON.stringify({
      message: "AI-approved debug export: " + name,
      content: b64(raw),
      ...(sha ? { sha } : {}),
    }),
  });
  if (!written.ok) return problem("GitHub rejected export (HTTP " + written.status + ").", 502);
  return null;
}
function chunkName(path, index) {
  return PREFIX + encodePath(path) + ".parts/" + String(index + 1).padStart(6, "0") + ".txt";
}
function manifestName(path) {
  return PREFIX + encodePath(path) + ".index.md";
}
function buildManifest(path, size, chunks, etag) {
  return [
    "# Exported debug log: " + path,
    "",
    "This document indexes a private, explicitly approved debug-log export.",
    "Original site path: /" + path,
    "Original byte size: " + size,
    "Original R2 ETag: " + etag,
    "Part count: " + chunks,
    "First chunk: " + chunkName(path, 0),
    "Last chunk: " + chunkName(path, chunks - 1),
    "",
    "Read chunk files in numerical order. The parts directory is:",
    PREFIX + path + ".parts/",
    "",
    "Each part is standalone UTF-8 text, around 512 KiB. Lines may span",
    "chunk boundaries. Use the GitHub connector to list this directory",
    "and open the specific parts needed to troubleshoot.",
    "",
  ].join("\n");
}
async function loadChunk(bucket, path, part, size) {
  const offset = part * CHUNK_BYTES;
  const length = Math.min(size - offset, CHUNK_BYTES + 4);
  if (length === 0) return new Uint8Array(0);
  const result = await bucket.get("files/" + path, { range: { offset, length } });
  if (!result) throw new Error("Source file disappeared during export.");
  const bytes = new Uint8Array(await result.arrayBuffer());
  if (bytes.length !== length) throw new Error("Incomplete ranged read.");
  let start = 0;
  if (offset) {
    while (start < bytes.length && (bytes[start] & 0xc0) === 0x80) start++;
  }
  let end = Math.min(CHUNK_BYTES, bytes.length);
  if (offset + end < size) {
    while (end < bytes.length && (bytes[end] & 0xc0) === 0x80) end++;
  }
  const partBytes = bytes.slice(start, end);
  const decoded = new TextDecoder("utf-8", { fatal: true }).decode(partBytes);
  if (decoded.includes("\u0000")) throw new Error("File contains binary data.");
  return partBytes;
}
export async function shareTextChunk(bucket, env, path, part, expectedEtag) {
  const repo = gitSettings(env);
  if (!Number.isSafeInteger(part) || part < 0) return problem("Invalid AI chunk number.", 400);
  const object = await bucket.head("files/" + path);
  if (!object) return problem("File not found.", 404);
  if (part !== 0 && (!expectedEtag || expectedEtag !== object.etag)) {
    return problem("Original file changed during sharing. Restart from part one.", 409);
  }
  const totalParts = Math.max(1, Math.ceil(object.size / CHUNK_BYTES));
  if (part >= totalParts) return problem("Chunk is past end of file.", 400);

  // Fail closed before every write: no public destination can receive private logs.
  const check = await github(env, repo, "");
  if (!check.ok) return problem("Unable to verify private GitHub repository.", 502);
  const info = await check.json();
  if (info.private !== true || info.archived === true) {
    return problem("GitHub destination is not a writable private repository.", 403);
  }

  let bytes;
  try { bytes = await loadChunk(bucket, path, part, object.size); }
  catch (cause) { return problem(cause.message || "Invalid UTF-8 text.", 415); }

  const remote = totalParts === 1 ? PREFIX + encodePath(path) : chunkName(path, part);
  const failed = await saveFile(env, repo, remote, bytes, path);
  if (failed) return failed;

  const last = part === totalParts - 1;
  const githubPath = totalParts === 1 ? remote : manifestName(path);
  if (last && totalParts > 1) {
    const manifestBytes = new TextEncoder().encode(buildManifest(path, object.size, totalParts, object.etag));
    const manifestFailure = await saveFile(env, repo, manifestName(path), manifestBytes, path + " index");
    if (manifestFailure) return manifestFailure;
  }
  if (last) {
    await bucket.put(SHARES + path, "", {
      httpMetadata: { contentType: "application/octet-stream" },
      customMetadata: { etag: object.etag, syncedAt: new Date().toISOString() },
    });
  }
  return json({
    shared: last, path, etag: object.etag, nextPart: part + 1, totalParts,
    githubPath, githubUrl: "https://github.com/" + repo + "/blob/HEAD/" + githubPath,
    note: "GitHub retains commit history after file deletion.",
  });
}
