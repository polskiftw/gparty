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
