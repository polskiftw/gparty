import APP_SCRIPT from "./app.js";
import APP_STYLE from "./style.css";

const INDEX_KEY = "gallery-index.json";
const TAG_INDEX_KEY = "_internal/tag-index-v1.json";
const SOURCE_CONFIG_KEY = "_internal/reddit-sources.json";
const SUBREDDIT_NAME_PATTERN = /^[A-Za-z0-9_]{3,21}$/;
const MAX_MANAGED_SOURCES = 500;
const MAX_SOURCE_REQUEST_BYTES = 1024;
const SOURCE_REQUEST_HEADER = "x-gparty-source-request";
const ALLOWED_EXTENSIONS = ["jpg", "jpeg", "png", "gif", "webp", "mp4", "m4v", "webm"];
const STILL_EXTENSIONS = new Set(["jpg", "jpeg", "png", "webp"]);
const CLIP_EXTENSIONS = new Set(["gif", "mp4", "m4v", "webm"]);
const MAX_TAG_SELECTION_CACHE_ENTRIES = 256;

function emptyBuckets() {
  return {
    all: [],
    stills: [],
    clips: [],
    ...Object.fromEntries(ALLOWED_EXTENSIONS.map((extension) => [extension, []])),
  };
}

function buildBuckets(items) {
  const buckets = emptyBuckets();
  for (const item of items) {
    const extension = String(item.ext || "").toLowerCase();
    buckets.all.push(item);
    if (STILL_EXTENSIONS.has(extension)) buckets.stills.push(item);
    if (CLIP_EXTENSIONS.has(extension)) buckets.clips.push(item);
    if (Object.hasOwn(buckets, extension)) buckets[extension].push(item);
  }
  return buckets;
}

function requestedBucketName(requested) {
  if (requested === "stills" || requested === "clips") return requested;
  if (ALLOWED_EXTENSIONS.includes(requested)) return requested;
  return "all";
}

let indexCache = { expires: 0, buckets: emptyBuckets() };
let tagIndexCache = {
  expires: 0,
  catalog: [],
  items: [],
  idsByName: new Map(),
  selectionCache: new Map(),
};

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/robots.txt") return robotsResponse();
    if (url.pathname === "/") return htmlResponse(renderAppHtml(env.CONTACT_EMAIL));
    if (url.pathname === "/style.css") return assetResponse(APP_STYLE, "text/css; charset=utf-8");
    if (url.pathname === "/app.js") return assetResponse(APP_SCRIPT, "text/javascript; charset=utf-8");
    if (url.pathname === "/api/tags") return tagCatalog(env);
    if (url.pathname === "/api/random") return randomMedia(request, env);
    if (url.pathname === "/api/sources") {
      if (request.method !== "POST") {
        return new Response(JSON.stringify({ error: "Method not allowed." }), {
          status: 405,
          headers: {
            "content-type": "application/json; charset=utf-8",
            "cache-control": "no-store",
            allow: "POST",
          },
        });
      }
      return addManagedSource(request, env);
    }

    if (url.pathname.startsWith("/media/")) {
      let key;
      try {
        key = decodeURIComponent(url.pathname.slice("/media/".length));
      } catch {
        return new Response("Bad media key", { status: 400 });
      }
      if (!key.startsWith("gallery/")) return new Response("Not found", { status: 404 });
      return serveMedia(request, env, key);
    }

    return new Response("Not found", { status: 404 });
  },
};

async function loadIndex(env) {
  const now = Date.now();
  if (indexCache.expires > now && indexCache.buckets.all.length) return indexCache;

  const object = await env.MEDIA_BUCKET.get(INDEX_KEY);
  if (!object) return { expires: now + 30_000, buckets: emptyBuckets() };

  let payload;
  try {
    payload = JSON.parse(await object.text());
  } catch {
    return { expires: now + 30_000, buckets: emptyBuckets() };
  }

  const rawItems = Array.isArray(payload) ? payload : payload.items;
  const items = Array.isArray(rawItems)
    ? rawItems.filter((item) => item && typeof item.key === "string" && item.key.startsWith("gallery/"))
    : [];
  indexCache = { expires: now + 60_000, buckets: buildBuckets(items) };
  return indexCache;
}

function emptyTagIndexCache(expires) {
  return {
    expires,
    catalog: [],
    items: [],
    idsByName: new Map(),
    selectionCache: new Map(),
  };
}

async function loadTagIndex(env) {
  const now = Date.now();
  if (tagIndexCache.expires > now) return tagIndexCache;

  const object = await env.MEDIA_BUCKET.get(TAG_INDEX_KEY);
  if (!object) {
    tagIndexCache = emptyTagIndexCache(now + 30_000);
    return tagIndexCache;
  }

  let payload;
  try {
    payload = JSON.parse(await object.text());
  } catch {
    tagIndexCache = emptyTagIndexCache(now + 30_000);
    return tagIndexCache;
  }

  const rawCatalog = Array.isArray(payload?.catalog) ? payload.catalog : [];
  const rawItems = Array.isArray(payload?.items) ? payload.items : [];
  if (payload?.version !== 1 || rawCatalog.length > 20_000 || rawItems.length > 250_000) {
    tagIndexCache = emptyTagIndexCache(now + 30_000);
    return tagIndexCache;
  }

  const catalog = rawCatalog
    .map((entry, id) => ({
      id,
      name: Array.isArray(entry) ? String(entry[0] || "") : "",
      count: Array.isArray(entry) ? Number(entry[1]) : 0,
    }))
    .filter((entry) => entry.name && Number.isSafeInteger(entry.count) && entry.count > 0);
  const validIds = new Set(catalog.map((entry) => entry.id));
  const items = rawItems
    .filter((entry) => (
      Array.isArray(entry)
      && entry.length === 3
      && typeof entry[0] === "string"
      && entry[0].startsWith("gallery/")
      && ALLOWED_EXTENSIONS.includes(String(entry[1]).toLowerCase())
      && Array.isArray(entry[2])
      && entry[2].length <= 512
      && entry[2].every((id) => Number.isSafeInteger(id) && validIds.has(id))
    ))
    .map((entry) => ({ key: entry[0], ext: String(entry[1]).toLowerCase(), tags: entry[2] }));

  tagIndexCache = {
    expires: now + 60_000,
    catalog,
    items,
    idsByName: new Map(catalog.map((entry) => [entry.name, entry.id])),
    selectionCache: new Map(),
    generatedAt: typeof payload.generated_at === "string" ? payload.generated_at : "",
  };
  return tagIndexCache;
}

function cacheTagSelection(tagIndex, cacheKey, choices) {
  if (tagIndex.selectionCache.size >= MAX_TAG_SELECTION_CACHE_ENTRIES) {
    const oldestKey = tagIndex.selectionCache.keys().next().value;
    if (oldestKey !== undefined) tagIndex.selectionCache.delete(oldestKey);
  }
  tagIndex.selectionCache.set(cacheKey, choices);
  return choices;
}

function taggedChoices(tagIndex, requestedIds, bucketName) {
  const sortedIds = [...requestedIds].sort((left, right) => left - right);
  const cacheKey = `${bucketName}:${sortedIds.join(",")}`;
  const cached = tagIndex.selectionCache.get(cacheKey);
  if (cached) return cached;

  const choices = tagIndex.items.filter((item) => {
    if (!sortedIds.every((id) => item.tags.includes(id))) return false;
    const extension = item.ext;
    if (bucketName === "stills") return STILL_EXTENSIONS.has(extension);
    if (bucketName === "clips") return CLIP_EXTENSIONS.has(extension);
    if (bucketName !== "all") return extension === bucketName;
    return true;
  });
  return cacheTagSelection(tagIndex, cacheKey, choices);
}

async function tagCatalog(env) {
  const tagIndex = await loadTagIndex(env);
  return jsonResponse({
    version: 1,
    generatedAt: tagIndex.generatedAt || "",
    tags: tagIndex.catalog,
  });
}

async function randomMedia(request, env) {
  const url = new URL(request.url);
  const requested = (url.searchParams.get("ext") || "all").toLowerCase();
  const bucketName = requestedBucketName(requested);
  const requestedTags = [...new Set(url.searchParams.getAll("tag"))];
  if (requestedTags.length > 32) {
    return jsonResponse({ error: "Too many tags are selected." }, 400);
  }
  if (requestedTags.some((name) => !name || name.length > 128)) {
    return jsonResponse({ error: "The selected tags are invalid." }, 400);
  }

  let choices;
  if (requestedTags.length) {
    const tagIndex = await loadTagIndex(env);
    if (requestedTags.some((name) => !tagIndex.idsByName.has(name))) {
      return jsonResponse({ error: "The selected tags are no longer available." }, 400);
    }
    const requestedIds = requestedTags.map((name) => tagIndex.idsByName.get(name));
    choices = taggedChoices(tagIndex, requestedIds, bucketName);
  } else {
    const index = await loadIndex(env);
    choices = index.buckets[bucketName];
  }

  if (!choices.length) {
    return jsonResponse({ error: "Nothing matches those tags." }, 404);
  }
  const item = choices[Math.floor(Math.random() * choices.length)];
  return jsonResponse({
    key: item.key,
    ext: item.ext,
    url: `/media/${encodeURIComponent(item.key)}`,
    total: choices.length,
  });
}

function normalizeSubredditName(value) {
  let candidate = String(value || "").trim();
  if (!candidate) return "";

  if (/^https?:\/\//i.test(candidate)) {
    let parsed;
    try {
      parsed = new URL(candidate);
    } catch {
      return "";
    }
    const hostname = parsed.hostname.toLowerCase();
    if (!["reddit.com", "www.reddit.com", "old.reddit.com", "new.reddit.com"].includes(hostname)) {
      return "";
    }
    const parts = parsed.pathname.split("/").filter(Boolean);
    if (parts.length < 2 || parts[0].toLowerCase() !== "r") return "";
    candidate = parts[1];
  } else {
    candidate = candidate.replace(/^\/+|\/+$/g, "");
    if (candidate.toLowerCase().startsWith("r/")) candidate = candidate.slice(2);
    candidate = candidate.split("/")[0];
  }

  return SUBREDDIT_NAME_PATTERN.test(candidate) ? candidate : "";
}

function hasVerifiedClientCertificate(request) {
  return request.cf?.tlsClientAuth?.certPresented === "1"
    && request.cf?.tlsClientAuth?.certVerified === "SUCCESS"
    && request.cf?.tlsClientAuth?.certRevoked === "0";
}

async function readManagedSources(env) {
  const object = await env.MEDIA_BUCKET.get(SOURCE_CONFIG_KEY);
  if (!object) return { object: null, sources: [] };

  let payload;
  try {
    payload = JSON.parse(await object.text());
  } catch {
    throw new Error("Private source configuration is unreadable.");
  }

  const sources = Array.isArray(payload) ? payload : payload?.sources;
  if (!Array.isArray(sources)) {
    throw new Error("Private source configuration has an invalid shape.");
  }

  const cleaned = [];
  const seen = new Set();
  for (const value of sources) {
    const name = normalizeSubredditName(value);
    const key = name.toLowerCase();
    if (!name || seen.has(key)) continue;
    seen.add(key);
    cleaned.push(name);
  }
  return { object, sources: cleaned };
}

async function readLimitedTextBody(request, maximumBytes) {
  const declaredLength = Number(request.headers.get("content-length") || "0");
  if (Number.isFinite(declaredLength) && declaredLength > maximumBytes) {
    return { tooLarge: true, text: "" };
  }
  if (!request.body) return { tooLarge: false, text: "" };

  const reader = request.body.getReader();
  const decoder = new TextDecoder("utf-8", { fatal: true });
  let total = 0;
  let text = "";

  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.byteLength;
      if (total > maximumBytes) {
        await reader.cancel();
        return { tooLarge: true, text: "" };
      }
      text += decoder.decode(value, { stream: true });
    }
    text += decoder.decode();
    return { tooLarge: false, text };
  } catch {
    try {
      await reader.cancel();
    } catch {
      // The stream may already be closed.
    }
    return { tooLarge: false, text: null };
  }
}

async function addManagedSource(request, env) {
  if (!hasVerifiedClientCertificate(request)) {
    return jsonResponse({ error: "A verified client certificate is required." }, 403);
  }
  if (request.headers.get(SOURCE_REQUEST_HEADER) !== "1") {
    return jsonResponse({ error: "This source request is not authorized." }, 403);
  }

  const contentType = (request.headers.get("content-type") || "")
    .split(";", 1)[0]
    .trim()
    .toLowerCase();
  if (contentType !== "application/json") {
    return jsonResponse({ error: "Expected a JSON request." }, 415);
  }

  const body = await readLimitedTextBody(request, MAX_SOURCE_REQUEST_BYTES);
  if (body.tooLarge) {
    return jsonResponse({ error: "That request is too large." }, 413);
  }
  if (body.text === null) {
    return jsonResponse({ error: "The subreddit request was unreadable." }, 400);
  }

  let payload;
  try {
    payload = JSON.parse(body.text);
  } catch {
    return jsonResponse({ error: "The subreddit request was unreadable." }, 400);
  }
  if (
    !payload
    || typeof payload !== "object"
    || Array.isArray(payload)
    || Object.keys(payload).length !== 1
    || typeof payload.subreddit !== "string"
  ) {
    return jsonResponse({ error: "The subreddit request was invalid." }, 400);
  }

  const name = normalizeSubredditName(payload.subreddit);
  if (!name) {
    return jsonResponse(
      { error: "Enter a subreddit name such as pics, r/pics, or a Reddit subreddit URL." },
      400,
    );
  }

  const wanted = name.toLowerCase();
  try {
    for (let attempt = 0; attempt < 5; attempt += 1) {
      const { object, sources } = await readManagedSources(env);
      if (sources.some((source) => source.toLowerCase() === wanted)) {
        return jsonResponse({ added: false, alreadyExists: true, count: sources.length });
      }
      if (sources.length >= MAX_MANAGED_SOURCES) {
        return jsonResponse({ error: "The private source list is full." }, 409);
      }

      sources.push(name);
      const storedBody = JSON.stringify({
        version: 1,
        updated_at: new Date().toISOString(),
        sources,
      }, null, 2) + "\n";
      const onlyIf = new Headers(
        object
          ? { "If-Match": object.httpEtag }
          : { "If-None-Match": "*" },
      );
      const stored = await env.MEDIA_BUCKET.put(SOURCE_CONFIG_KEY, storedBody, {
        onlyIf,
        httpMetadata: {
          contentType: "application/json; charset=utf-8",
          cacheControl: "no-store",
        },
        customMetadata: {
          private: "true",
          purpose: "reddit-sources",
        },
      });
      if (stored) {
        return jsonResponse({ added: true, alreadyExists: false, count: sources.length }, 201);
      }
    }
  } catch (problem) {
    console.error("Private source update failed", problem);
    return jsonResponse({ error: "The private source list is temporarily unavailable." }, 503);
  }

  return jsonResponse(
    { error: "The source list changed at the same moment. Please tap Add once more." },
    409,
  );
}

async function serveMedia(request, env, key) {
  const range = request.headers.get("Range");
  const object = await env.MEDIA_BUCKET.get(key, range ? { range: request.headers } : undefined);
  if (!object) return new Response("Not found", { status: 404 });

  const headers = new Headers();
  object.writeHttpMetadata(headers);
  headers.set("etag", object.httpEtag);
  headers.set("cache-control", "private, max-age=86400");
  headers.set("accept-ranges", "bytes");

  const status = object.range ? 206 : 200;
  if (object.range) {
    const offset = object.range.offset ?? 0;
    const length = object.range.length ?? object.size;
    headers.set("content-range", `bytes ${offset}-${offset + length - 1}/${object.size}`);
    headers.set("content-length", String(length));
  }
  return new Response(object.body, { status, headers });
}

function robotsResponse() {
  return new Response("User-agent: *\nDisallow: /\n", {
    headers: {
      "content-type": "text/plain; charset=utf-8",
      "cache-control": "public, max-age=86400",
    },
  });
}

function htmlResponse(body) {
  return new Response(body, {
    headers: {
      "content-type": "text/html; charset=utf-8",
      "cache-control": "no-store",
    },
  });
}

function assetResponse(body, contentType) {
  return new Response(body, {
    headers: {
      "content-type": contentType,
      "cache-control": "no-cache",
    },
  });
}

function jsonResponse(value, status = 200) {
  return new Response(JSON.stringify(value), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
    },
  });
}

function escapeHtmlAttribute(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll('"', "&quot;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

function renderAppHtml(contactEmail) {
  const email = String(contactEmail || "").trim();
  if (!email) throw new Error("CONTACT_EMAIL secret is missing");
  const escapedEmail = escapeHtmlAttribute(email);
  return APP_HTML
    .replaceAll("__CONTACT_EMAIL_HREF__", `mailto:${escapedEmail}`)
    .replaceAll("__CONTACT_EMAIL_LABEL__", `Email ${escapedEmail}`);
}

const APP_HTML = String.raw`<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
  <title>GParty</title>
  <link rel="stylesheet" href="/style.css">
  <script src="/app.js" defer></script>
</head>
<body>
  <aside id="tag-sidebar" aria-labelledby="tag-sidebar-title">
    <div id="tag-sidebar-title">Tags</div>
    <div id="tag-sidebar-state">Loading tags…</div>
    <div id="tag-list"></div>
  </aside>
  <main id="stage" title="Click media to toggle fit/native size"></main>
  <div id="bar">
    <button id="next" type="button">Next random</button>
    <div id="footer-controls">
      <select id="filter" aria-label="Media filter">
        <option value="all">All</option>
        <option value="stills">Stills</option>
        <option value="clips">Clips</option>
      </select>
      <button id="add-source-open" class="icon-action" type="button" aria-label="Add a subreddit">
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 5v14M5 12h14"/></svg>
      </button>
      <a class="icon-link" href="https://github.com/polskiftw/gparty" target="_blank" rel="noopener noreferrer" aria-label="Open GitHub repository">
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 .7a11.5 11.5 0 0 0-3.64 22.41c.58.11.79-.25.79-.56v-2.03c-3.22.7-3.9-1.37-3.9-1.37-.53-1.34-1.29-1.7-1.29-1.7-1.05-.72.08-.71.08-.71 1.17.08 1.78 1.2 1.78 1.2 1.04 1.78 2.72 1.27 3.39.97.1-.75.4-1.27.74-1.56-2.57-.29-5.27-1.29-5.27-5.73 0-1.27.45-2.3 1.2-3.11-.12-.3-.52-1.48.11-3.08 0 0 .98-.31 3.16 1.19a10.9 10.9 0 0 1 5.75 0c2.18-1.5 3.16-1.19 3.16-1.19.63 1.6.23 2.78.11 3.08.74.81 1.2 1.84 1.2 3.11 0 4.45-2.71 5.43-5.29 5.72.42.36.79 1.06.79 2.14v3.16c0 .31.21.68.8.56A11.5 11.5 0 0 0 12 .7Z"/></svg>
      </a>
      <a class="icon-link" href="__CONTACT_EMAIL_HREF__" aria-label="__CONTACT_EMAIL_LABEL__">
        <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M2.75 4.5h18.5A2.75 2.75 0 0 1 24 7.25v9.5a2.75 2.75 0 0 1-2.75 2.75H2.75A2.75 2.75 0 0 1 0 16.75v-9.5A2.75 2.75 0 0 1 2.75 4.5Zm0 1.75a1 1 0 0 0-.64.23L12 14.58l9.89-8.1a1 1 0 0 0-.64-.23H2.75Zm19.5 2.03-6.87 5.63 6.64 4.01c.15-.34.23-.72.23-1.17V8.28ZM1.75 8.28v8.47c0 .45.08.83.23 1.17l6.64-4.01-6.87-5.63Zm8.31 6.8-6.7 4.05c.2.08.42.12.64.12h16c.22 0 .44-.04.64-.12l-6.7-4.05-1.38 1.13a.88.88 0 0 1-1.12 0l-1.38-1.13Z"/></svg>
      </a>
    </div>
    <span id="status"></span>
  </div>
  <div id="error"></div>
  <dialog id="source-dialog" aria-labelledby="source-dialog-title">
    <form id="source-dialog-body" method="post" action="/api/sources" accept-charset="UTF-8">
      <div id="source-dialog-title">Add subreddit</div>
      <label for="source-input">Subreddit name</label>
      <input id="source-input" name="subreddit" type="text" maxlength="128" placeholder="pics" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false" required>
      <div id="source-feedback" aria-live="polite"></div>
      <div id="source-actions">
        <button id="source-close" type="button">Cancel</button>
        <button id="source-add" type="submit">Add</button>
      </div>
    </form>
  </dialog>
</body>
</html>`;