// Only explicitly approved UTF-8 logs travel to a separate PRIVATE GitHub repo.
// Each request streams just one readable UTF-8 chunk; completion makes a single
// atomic Git commit (rather than thousands of file commits).
const CHUNK_BYTES = 512 * 1024;
const SHARES = "ai-shares/";
const PREFIX = "shared/";

function json(value, code = 200) {
  return new Response(JSON.stringify(value), {
    status: code, headers: { "content-type": "application/json; charset=utf-8", "cache-control": "private, no-store" },
  });
}
function problem(message, status) { return json({ error: message }, status); }
function encoded(path) { return path.split("/").map(encodeURIComponent).join("/"); }
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
async function jsonCall(env, repo, route, options = {}) {
  const result = await github(env, repo, route, options);
  if (!result.ok) throw new Error("GitHub API " + result.status + " for " + route.split("/").slice(-1)[0]);
  return await result.json();
}
function b64(data) {
  let binary = "";
  for (let i = 0; i < data.length; i += 32768) {
    binary += String.fromCharCode(...data.subarray(i, i + 32768));
  }
  return btoa(binary);
}
function chunkName(path, index) {
  return PREFIX + path + ".parts/" + String(index + 1).padStart(6, "0") + ".txt";
}
function manifestName(path) { return PREFIX + path + ".index.md"; }
function buildManifest(path, size, count, etag) {
  return [
    "# Debug log: " + path,
    "",
    "Private, user-approved export from gooning.party.",
    "Original path: /" + path,
    "Original file byte size: " + size,
    "Original R2 ETag: " + etag,
    "Number of text chunks: " + count,
    "Chunks directory: " + PREFIX + path + ".parts/",
    "First chunk: " + chunkName(path, 0),
    "Last chunk: " + chunkName(path, count - 1),
    "",
    "Read the files in numerical order. Each part is independently readable",
    "UTF-8 text of about 512 KiB. A line can span two neighboring parts.",
    "Use your connected private GitHub repository to browse and open them.",
    "",
  ].join("\n");
}
async function verifyPrivateRepo(env) {
  const repo = gitSettings(env);
  const response = await github(env, repo, "");
  if (!response.ok) throw new Error("Cannot verify GitHub repository privacy.");
  const details = await response.json();
  if (details.private !== true || details.archived === true) {
    throw new Error("GitHub destination must be private and writable.");
  }
  if (typeof details.default_branch !== "string" || !details.default_branch) {
    throw new Error("Initialize the private GitHub repository with a default branch.");
  }
  return { repo, branch: details.default_branch };
}
async function makeBlob(env, repo, bytes) {
  const created = await jsonCall(env, repo, "/git/blobs", {
    method: "POST", body: JSON.stringify({ content: b64(bytes), encoding: "base64" }),
  });
  if (!/^[0-9a-f]{40}$/i.test(created.sha || "")) throw new Error("GitHub did not return a blob hash.");
  return created.sha;
}
async function loadChunk(bucket, path, part, size) {
  const offset = part * CHUNK_BYTES;
  const length = Math.min(size - offset, CHUNK_BYTES + 4);
  if (!length) return new Uint8Array(0);
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
  if (!Number.isSafeInteger(part) || part < 0) return problem("Invalid chunk number.", 400);
  const object = await bucket.head("files/" + path);
  if (!object) return problem("File not found.", 404);
  if (part && (!expectedEtag || expectedEtag !== object.etag)) {
    return problem("Original file changed during sharing. Restart export.", 409);
  }
  const count = Math.max(1, Math.ceil(object.size / CHUNK_BYTES));
  if (part >= count) return problem("Part is beyond end of file.", 400);
  let verified, bytes;
  try {
    verified = await verifyPrivateRepo(env);
    bytes = await loadChunk(bucket, path, part, object.size);
  } catch (cause) { return problem(cause.message || "Invalid text.", 422); }
  try {
    const sha = await makeBlob(env, verified.repo, bytes);
    return json({
      blobSha: sha, etag: object.etag, nextPart: part + 1, totalParts: count,
      awaitingCommit: true,
    });
  } catch (cause) { return problem(cause.message || "GitHub upload failed.", 502); }
}
export async function finishTextShare(bucket, env, path, expectedEtag, parts) {
  const object = await bucket.head("files/" + path);
  if (!object) return problem("File not found.", 404);
  if (!expectedEtag || expectedEtag !== object.etag) {
    return problem("Original file changed. Restart the export.", 409);
  }
  const count = Math.max(1, Math.ceil(object.size / CHUNK_BYTES));
  if (!Array.isArray(parts) || parts.length !== count ||
    parts.some((sha) => typeof sha !== "string" || !/^[0-9a-f]{40}$/i.test(sha))) {
    return problem("Incomplete or invalid GitHub chunk receipts.", 400);
  }
  let verified;
  try { verified = await verifyPrivateRepo(env); }
  catch (cause) { return problem(cause.message, 403); }
  const { repo, branch } = verified;
  const entries = parts.map((sha, index) => ({
    path: count === 1 ? PREFIX + path : chunkName(path, index),
    mode: "100644", type: "blob", sha,
  }));
  const remotePath = count === 1 ? PREFIX + path : manifestName(path);
  try {
    if (count > 1) {
      const manifest = new TextEncoder().encode(buildManifest(path, object.size, count, object.etag));
      const manifestSha = await makeBlob(env, repo, manifest);
      entries.push({ path: manifestName(path), mode: "100644", type: "blob", sha: manifestSha });
    }
    for (let retry = 0; retry < 3; retry++) {
      const refName = "/git/ref/heads/" + encoded(branch);
      const ref = await jsonCall(env, repo, refName);
      const parent = ref.object?.sha;
      if (!parent) throw new Error("Cannot read the GitHub branch head.");
      const commit = await jsonCall(env, repo, "/git/commits/" + parent);
      const base = commit.tree?.sha;
      if (!base) throw new Error("Cannot read the GitHub parent tree.");
      const tree = await jsonCall(env, repo, "/git/trees", {
        method: "POST", body: JSON.stringify({ base_tree: base, tree: entries }),
      });
      const next = await jsonCall(env, repo, "/git/commits", {
        method: "POST", body: JSON.stringify({
          message: "Share approved debug log: " + path,
          tree: tree.sha,
          parents: [parent],
        }),
      });
      const moved = await github(env, repo, "/git/refs/heads/" + encoded(branch), {
        method: "PATCH", body: JSON.stringify({ sha: next.sha, force: false }),
      });
      if (moved.ok) {
        await bucket.put(SHARES + path, "", {
          httpMetadata: { contentType: "application/octet-stream" },
          customMetadata: { etag: object.etag, syncedAt: new Date().toISOString() },
        });
        return json({
          shared: true, path, totalParts: count, githubPath: remotePath,
          githubUrl: "https://github.com/" + repo + "/blob/" + encoded(branch) + "/" + encoded(remotePath),
          note: "GitHub commit history retains earlier versions.",
        });
      }
      if (moved.status !== 409 && moved.status !== 422) {
        throw new Error("GitHub rejected the branch update (HTTP " + moved.status + ").");
      }
    }
    throw new Error("The GitHub branch is busy. Retry the final commit.");
  } catch (cause) { return problem(cause.message || "GitHub finalize failed.", 502); }
}
