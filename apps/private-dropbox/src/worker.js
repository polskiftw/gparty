import { PAGE_HTML } from "./page.js";
import APP_JS from "./app.js";
import APP_CSS from "./style.css";

export function authenticated(request) {
  const cert = request.cf?.tlsClientAuth;
  return cert?.certPresented === "1" && cert?.certVerified === "SUCCESS" && cert?.certRevoked === "0";
}

const FILES = "files/";
const FOLDERS = "folders/";
const SHARES = "ai-shares/";
const MAX_UPLOAD = 64 * 1024 * 1024;
const MAX_AI_FILE = 4 * 1024 * 1024;
const TEXT_EXTENSIONS = new Set("txt log md json toml yaml yml ini cfg conf config csv xml trace stack diff patch out err cs js ts py sh java go rs sql".split(" "));
const SAFE_IMAGES = new Map([["png","image/png"],["jpg","image/jpeg"],["jpeg","image/jpeg"],["gif","image/gif"],["webp","image/webp"]]);
const RESERVED_ROOTS = new Set("_ gallery api media style.css app.js robots.txt favicon.ico favicon.svg favicon-16x16.png favicon-32x32.png apple-touch-icon.png site.webmanifest".split(" "));
function json(value, status = 200) {
  return new Response(JSON.stringify(value), { status, headers: {
    "content-type": "application/json; charset=utf-8", "cache-control": "private, no-store" } });
}
function error(message, status) { return json({ error: message }, status); }
function parsePath(value, allowEmpty = false) {
  if (typeof value !== "string" || (!value && !allowEmpty) || value.startsWith("/")
    || value.endsWith("/") || value.includes("\\")
    || new TextEncoder().encode(value).byteLength > 900) throw new Error("Invalid file path.");
  if (!value) return "";
  const segments = value.split("/");
  if (segments.length > 16 || segments.some((part) => !part || part === "." || part === ".."
    || part.length > 200 || /[\u0000-\u001f\u007f]/u.test(part))) throw new Error("Invalid file path.");
  if (RESERVED_ROOTS.has(segments[0].toLowerCase())) throw new Error("That name is reserved.");
  return value;
}
function parentPath(path) { return path.includes("/") ? path.slice(0, path.lastIndexOf("/")) : ""; }
function nameOf(path) { return path.split("/").at(-1); }
function extension(path) { const name = nameOf(path); return name.includes(".") ? name.split(".").at(-1).toLowerCase() : ""; }
function isText(path) { return TEXT_EXTENSIONS.has(extension(path)); }
function encodePath(path) { return path.split("/").map(encodeURIComponent).join("/"); }
function canWrite(request) {
  const origin = request.headers.get("origin");
  return request.headers.get("x-gparty-write") === "1"
    && (!origin || origin === new URL(request.url).origin)
    && request.headers.get("sec-fetch-site") !== "cross-site";
}

async function readJson(request) {
  const size = Number(request.headers.get("content-length") || 0);
  if (size > 4096) throw new Error("Request is too large.");
  if (!request.body) throw new Error("A JSON body is required.");
  const reader = request.body.getReader();
  const parts = [];
  let total = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.byteLength;
      if (total > 4096) throw new Error("Request is too large.");
      parts.push(value);
    }
  } finally { reader.releaseLock(); }
  const bytes = new Uint8Array(total);
  let offset = 0;
  for (const part of parts) { bytes.set(part, offset); offset += part.length; }
  const parsed = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) throw new Error("Expected a JSON object.");
  return parsed;
}
async function folderExists(bucket, folder) {
  return !folder || Boolean(await bucket.head(FOLDERS + folder + "/"));
}
async function listing(bucket, folder) {
  if (!(await folderExists(bucket, folder))) return error("Folder not found.", 404);
  const prefix = folder ? folder + "/" : "";
  const [storedFiles, storedDirs, storedShares] = await Promise.all([
    bucket.list({ prefix: FILES + prefix, delimiter: "/", limit: 1000 }),
    bucket.list({ prefix: FOLDERS + prefix, delimiter: "/", limit: 1000 }),
    bucket.list({ prefix: SHARES + prefix, delimiter: "/", limit: 1000, include: ["customMetadata"] }),
  ]);
  const allFolders = new Set(
    [...storedFiles.delimitedPrefixes, ...storedDirs.delimitedPrefixes]
      .map((key) => key.replace(/^(files|folders)\//, "").slice(prefix.length).replace(/\/$/, ""))
      .filter((name) => name && !name.includes("/")),
  );
  const shares = new Map(storedShares.objects.map((item) => [
    item.key.slice((SHARES + prefix).length), item.customMetadata?.etag,
  ]));
  const files = storedFiles.objects.map((item) => {
    const name = item.key.slice((FILES + prefix).length);
    if (!name || name.includes("/")) return null;
    return {
      name, size: item.size, modified: item.uploaded?.toISOString?.() || "",
      ai: shares.has(name) && shares.get(name) === item.etag, text: isText(name),
    };
  }).filter(Boolean);
  return json({
    folder, folders: [...allFolders].sort((a, b) => a.localeCompare(b)),
    files: files.sort((a, b) => a.name.localeCompare(b.name)),
    truncated: storedFiles.truncated || storedDirs.truncated || storedShares.truncated,
  });
}

async function newFolder(request, bucket) {
  const payload = await readJson(request);
  const folder = parsePath(payload.path);
  if (!(await folderExists(bucket, parentPath(folder)))) return error("Parent folder not found.", 404);
  if (await bucket.head(FILES + folder)) return error("A file already has that name.", 409);
  const written = await bucket.put(FOLDERS + folder + "/", "", {
    onlyIf: new Headers({ "If-None-Match": "*" }),
    httpMetadata: { contentType: "application/octet-stream" },
  });
  if (!written) return error("Folder already exists.", 409);
  return json({ created: true, path: folder }, 201);
}
async function upload(request, bucket, path) {
  if (!request.body) return error("Missing file contents.", 400);
  const rawSize = request.headers.get("content-length");
  const size = rawSize === null ? NaN : Number(rawSize);
  if (!Number.isSafeInteger(size) || size < 0) return error("Upload must have a known Content-Length.", 411);
  if (size > MAX_UPLOAD) return error("File is larger than 64 MiB.", 413);
  if (!(await folderExists(bucket, parentPath(path)))) return error("Parent folder not found.", 404);
  if (await folderExists(bucket, path)) return error("A folder already has that name.", 409);
  const written = await bucket.put(FILES + path, request.body, {
    onlyIf: new Headers({ "If-None-Match": "*" }),
    httpMetadata: { contentType: "application/octet-stream", cacheControl: "private, no-store" },
    customMetadata: { originalName: nameOf(path) },
  });
  if (!written) return error("File exists. Rename before uploading.", 409);
  return json({ uploaded: true, path, size }, 201);
}
function fileHeaders(path, object) {
  const ext = extension(path);
  const headers = new Headers({
    "cache-control": "private, no-store",
    "etag": object.httpEtag,
    "accept-ranges": "bytes",
  });
  const image = SAFE_IMAGES.get(ext);
  headers.set("content-type", image || (isText(path) ? "text/plain; charset=utf-8" : "application/octet-stream"));
  headers.set("content-disposition",
    (image || isText(path) ? "inline" : "attachment") +
    "; filename*=UTF-8''" + encodeURIComponent(nameOf(path)));
  return headers;
}
async function serveFile(request, bucket, path) {
  const ranged = request.headers.has("range");
  const object = request.method === "HEAD"
    ? await bucket.head(FILES + path)
    : await bucket.get(FILES + path, ranged ? { range: request.headers } : undefined);
  if (!object) return new Response("Not found.", { status: 404 });
  const headers = fileHeaders(path, object);
  let status = 200;
  if (ranged && object.range) {
    status = 206;
    const offset = object.range.offset ?? 0;
    const length = object.range.length ?? object.size;
    headers.set("content-range", "bytes " + offset + "-" + (offset + length - 1) + "/" + object.size);
    headers.set("content-length", String(length));
  } else headers.set("content-length", String(object.size));
  return new Response(request.method === "HEAD" ? null : object.body, { status, headers });
}
async function preview(bucket, path) {
  if (!isText(path)) return error("Only text files can be previewed.", 415);
  const head = await bucket.head(FILES + path);
  if (!head) return error("File not found.", 404);
  const bytes = Math.min(head.size, 128 * 1024);
  const partial = bytes ? await bucket.get(FILES + path, { range: { offset: 0, length: bytes } }) : null;
  return json({ path, text: partial ? await partial.text() : "", truncated: head.size > bytes, size: head.size });
}

function githubSettings(env) {
  const identifier = String(env.AI_GITHUB_REPO || "");
  if (!/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(identifier) || !env.AI_GITHUB_TOKEN) {
    throw new Error("The private GitHub bridge is not configured.");
  }
  return identifier;
}
async function githubCall(env, repo, path, options = {}) {
  return fetch("https://api.github.com/repos/" + repo + path, {
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
function base64(bytes) {
  let binary = "";
  for (let i = 0; i < bytes.length; i += 32768) {
    binary += String.fromCharCode(...bytes.subarray(i, i + 32768));
  }
  return btoa(binary);
}
async function shareWithAi(request, bucket, env) {
  const payload = await readJson(request);
  const path = parsePath(payload.path);
  if (!isText(path)) return error("Only text, code, and logs can be shared with AI.", 415);
  const repo = githubSettings(env);
  const object = await bucket.get(FILES + path);
  if (!object) return error("File not found.", 404);
  if (object.size > MAX_AI_FILE) return error("AI bridge limit is 4 MiB per file.", 413);
  const bytes = new Uint8Array(await object.arrayBuffer());
  let decoded;
  try { decoded = new TextDecoder("utf-8", { fatal: true }).decode(bytes); }
  catch { return error("File is not valid UTF-8 text.", 415); }
  if (decoded.includes("\u0000")) return error("File contains binary data.", 415);

  // Fail closed: a misconfigured public GitHub repository must never receive private logs.
  const verification = await githubCall(env, repo, "");
  if (!verification.ok) return error("Unable to verify the GitHub destination.", 502);
  const info = await verification.json();
  if (info.private !== true || info.archived === true) {
    return error("GitHub destination is not a writable private repository. Nothing was shared.", 403);
  }
  const remotePath = "shared/" + encodePath(path);
  const existingResponse = await githubCall(env, repo, "/contents/" + remotePath);
  let sha;
  if (existingResponse.ok) {
    const existing = await existingResponse.json();
    if (existing.type !== "file" || typeof existing.sha !== "string") {
      return error("GitHub target is not a normal file.", 409);
    }
    sha = existing.sha;
  } else if (existingResponse.status !== 404) {
    return error("Could not inspect the GitHub destination.", 502);
  }
  const saved = await githubCall(env, repo, "/contents/" + remotePath, {
    method: "PUT",
    body: JSON.stringify({
      message: "Sync approved debug file: " + path,
      content: base64(bytes),
      ...(sha ? { sha } : {}),
    }),
  });
  if (!saved.ok) return error("GitHub rejected the sync (HTTP " + saved.status + ").", 502);
  const response = await saved.json();
  await bucket.put(SHARES + path, "", {
    httpMetadata: { contentType: "application/octet-stream" },
    customMetadata: { etag: object.etag, syncedAt: new Date().toISOString() },
  });
  return json({
    shared: true, path, githubPath: "shared/" + path,
    githubUrl: response.content?.html_url || "",
    note: "GitHub preserves commit history even after a file is deleted.",
  });
}

function staticAsset(contents, type) {
  return new Response(contents, { headers: {
    "content-type": type, "cache-control": "private, no-store" } });
}

export default {
  async fetch(request, env) {
    if (!authenticated(request)) return error("A verified client certificate is required.", 403);
    if (!env.PRIVATE_FILES_BUCKET) return error("Private storage is not configured.", 503);
    const url = new URL(request.url);
    const pathname = url.pathname;
    try {
      if (pathname === "/_/app.js" && request.method === "GET") {
        return staticAsset(APP_JS, "text/javascript; charset=utf-8");
      }
      if (pathname === "/_/style.css" && request.method === "GET") {
        return staticAsset(APP_CSS, "text/css; charset=utf-8");
      }
      if (pathname.startsWith("/_/api/")) {
        const op = pathname.slice("/_/api/".length);
        if (request.method === "GET") {
          if (op === "list") {
            return listing(env.PRIVATE_FILES_BUCKET, parsePath(url.searchParams.get("path") || "", true));
          }
          if (op === "preview") {
            return preview(env.PRIVATE_FILES_BUCKET, parsePath(url.searchParams.get("path")));
          }
          if (op === "config") {
            return json({ aiEnabled: Boolean(env.AI_GITHUB_REPO && env.AI_GITHUB_TOKEN), maxUploadMiB: 64 });
          }
        } else if (request.method === "POST") {
          if (!canWrite(request)) return error("Same-origin write authorization required.", 403);
          if (op === "upload") {
            return upload(request, env.PRIVATE_FILES_BUCKET, parsePath(url.searchParams.get("path")));
          }
          if (op === "folder") return newFolder(request, env.PRIVATE_FILES_BUCKET);
          if (op === "share") return shareWithAi(request, env.PRIVATE_FILES_BUCKET, env);
        }
        return error("Not found.", 404);
      }
      if (request.method !== "GET" && request.method !== "HEAD") return error("Method not allowed.", 405);
      if (pathname === "/" || pathname.endsWith("/")) {
        if (pathname !== "/") {
          const folder = parsePath(decodeURIComponent(pathname.slice(1, -1)));
          if (!(await folderExists(env.PRIVATE_FILES_BUCKET, folder))) {
            return new Response("Folder not found.", { status: 404 });
          }
        }
        return staticAsset(PAGE_HTML, "text/html; charset=utf-8");
      }
      return serveFile(request, env.PRIVATE_FILES_BUCKET, parsePath(decodeURIComponent(pathname.slice(1))));
    } catch (problem) {
      if (problem instanceof URIError || problem instanceof SyntaxError
        || problem.message?.includes("Invalid file path")
        || problem.message?.includes("reserved")
        || problem.message?.includes("Request is too large")
        || problem.message?.includes("Expected a JSON")
        || problem.message?.includes("JSON body")) {
        return error(problem.message || "Invalid request.", 400);
      }
      if (problem.message?.includes("bridge is not configured")) return error(problem.message, 503);
      console.error("Private dropbox request failed", problem?.name || "Error");
      return error("Operation temporarily unavailable.", 503);
    }
  },
};
