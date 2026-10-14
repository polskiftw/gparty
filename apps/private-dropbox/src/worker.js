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
      .map((key) => key.replace(/^(files|folders)\//, "").slice(prefix.length))
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
