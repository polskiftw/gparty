// Server-side Reddit post lookup and allowlisted media proxy.
// No browser cookies, local services, or third-party archive providers.
const REDDIT_HOSTS = new Set(["reddit.com", "www.reddit.com", "old.reddit.com", "np.reddit.com", "m.reddit.com", "redd.it"]);
const MEDIA_HOSTS = new Set(["i.redd.it", "preview.redd.it", "external-preview.redd.it", "v.redd.it", "packaged-media.redd.it", "i.imgur.com"]);
const MAX_MEDIA_BYTES = 512 * 1024 * 1024;
let tokenCache = { identity: "", token: "", expires: 0 };

export class ArchiveError extends Error {
  constructor(message, status = 400) { super(message); this.name = "ArchiveError"; this.status = status; }
}
function safeUrl(input) {
  if (typeof input !== "string" || input.length > 2048) throw new ArchiveError("Paste a Reddit post URL.");
  let url;
  try { url = new URL(/^https?:\/\//i.test(input.trim()) ? input.trim() : "https://" + input.trim()); }
  catch { throw new ArchiveError("Enter a valid Reddit post URL."); }
  if (url.protocol !== "https:" || url.username || url.password || url.port) throw new ArchiveError("Only HTTPS Reddit post links are supported.");
  return url;
}
function redditHost(host) { return REDDIT_HOSTS.has(host.toLowerCase()); }
export function parseRedditId(input) {
  const url = safeUrl(input);
  if (!redditHost(url.hostname)) throw new ArchiveError("Only reddit.com and redd.it post links are supported.");
  let match;
  if (url.hostname === "redd.it") match = url.pathname.match(/^\/([a-z0-9]{4,16})\/?$/i);
  else match = url.pathname.match(/^\/(?:(?:r\/[^/]+\/)?comments|gallery)\/([a-z0-9]{4,16})(?:\/|$)/i);
  if (match) return match[1].toLowerCase();
  throw new ArchiveError("This link does not contain a Reddit post ID. Use Share > Copy link on a post.");
}
async function resolveRedditId(input) {
  try { return parseRedditId(input); }
  catch (problem) {
    if (!(problem instanceof ArchiveError) || !/does not contain a Reddit post ID/.test(problem.message)) throw problem;
  }
  const url = safeUrl(input);
  if (!redditHost(url.hostname) || !/^\/r\/[^/]+\/s\/[A-Za-z0-9_-]+\/?$/.test(url.pathname)) {
    throw new ArchiveError("Use a Reddit post link rather than a community, user profile, or search URL.");
  }
  let current = url.toString();
  for (let i = 0; i < 4; i++) {
    const response = await fetch(current, { method: "GET", redirect: "manual", headers: { "user-agent": "web:gooning.party-archiver:1.0 (private personal archiver)" } });
    if (response.status < 300 || response.status > 399) break;
    const location = response.headers.get("location");
    if (!location) break;
    const next = new URL(location, current);
    if (!redditHost(next.hostname) || next.protocol !== "https:") break;
    try { return parseRedditId(next.toString()); } catch { current = next.toString(); }
  }
  throw new ArchiveError("Reddit did not resolve this share link. Paste the full /comments/ post URL instead.", 422);
}
function userAgent(env) {
  return env.REDDIT_USER_AGENT || "web:gooning.party-archiver:1.0 (private personal archiver)";
}
async function oauthToken(env) {
  if (!env.REDDIT_CLIENT_ID || !env.REDDIT_CLIENT_SECRET) return null;
  const identity = env.REDDIT_CLIENT_ID + ":" + Boolean(env.REDDIT_REFRESH_TOKEN);
  if (tokenCache.identity === identity && tokenCache.expires > Date.now() + 60_000) return tokenCache.token;
  const tokenBody = env.REDDIT_REFRESH_TOKEN
    ? new URLSearchParams({ grant_type: "refresh_token", refresh_token: env.REDDIT_REFRESH_TOKEN })
    : new URLSearchParams({ grant_type: "client_credentials" });
  const response = await fetch("https://www.reddit.com/api/v1/access_token", {
    method: "POST",
    headers: {
      "authorization": "Basic " + btoa(env.REDDIT_CLIENT_ID + ":" + env.REDDIT_CLIENT_SECRET),
      "content-type": "application/x-www-form-urlencoded",
      "user-agent": userAgent(env),
    },
    body: tokenBody,
  });
  if (!response.ok) throw new ArchiveError("Reddit OAuth failed (HTTP " + response.status + "). Check the optional Worker secrets.", 502);
  const data = await response.json();
  if (!data.access_token || !Number.isFinite(Number(data.expires_in))) throw new ArchiveError("Reddit did not issue an access token.", 502);
  tokenCache = { identity, token: data.access_token, expires: Date.now() + Number(data.expires_in) * 1000 };
  return data.access_token;
}
async function fetchRedditPost(id, env) {
  const token = await oauthToken(env);
  const host = token ? "oauth.reddit.com" : "www.reddit.com";
  const response = await fetch("https://" + host + "/comments/" + id + ".json?raw_json=1&limit=0", {
    headers: {
      "accept": "application/json",
      "user-agent": userAgent(env),
      ...(token ? { "authorization": "Bearer " + token } : {}),
    },
    redirect: "manual",
  });
  if (response.status === 401 || response.status === 403 || response.status === 429) {
    throw new ArchiveError(
      "Reddit refused the post lookup (HTTP " + response.status + "). Hosted servers may need approved Reddit OAuth credentials configured as Cloudflare Worker secrets.",
      502,
    );
  }
  if (!response.ok) throw new ArchiveError("Reddit returned HTTP " + response.status + " for this post.", 502);
  const size = Number(response.headers.get("content-length") || 0);
  if (size > 4 * 1024 * 1024) throw new ArchiveError("Post metadata is unexpectedly large.", 502);
  const data = await response.json().catch(() => null);
  const post = data?.[0]?.data?.children?.find((item) => item.kind === "t3")?.data;
  if (!post || String(post.id).toLowerCase() !== id) {
    throw new ArchiveError("This post was not found or cannot be viewed with the current Reddit access.", 404);
  }
  return post;
}
function decodeEntities(url) { return String(url || "").replaceAll("&amp;", "&"); }
export function permittedMedia(input) {
  let url;
  try { url = new URL(decodeEntities(input)); }
  catch { throw new ArchiveError("Invalid media URL.", 400); }
  const host = url.hostname.toLowerCase();
  if (url.protocol !== "https:" || url.port || url.username || url.password ||
    !(MEDIA_HOSTS.has(host) || host.endsWith(".redditmedia.com"))) {
    throw new ArchiveError("Media origin is not on the approved CDN list.", 400);
  }
  return url;
}
function suffix(url, fallback = "bin") {
  const ext = new URL(url).pathname.match(/\.([a-z0-9]{2,5})$/i)?.[1]?.toLowerCase();
  return ext && ["jpg", "jpeg", "png", "webp", "gif", "mp4", "m4a", "webm", "mpd"].includes(ext) ? ext : fallback;
}
function safeSlug(title) {
  return String(title || "post").normalize("NFKD").replace(/[^a-zA-Z0-9_-]+/g, "-")
    .replace(/^-+|-+$/g, "").slice(0, 55) || "post";
}
export function planMedia(post) {
  const source = post.crosspost_parent_list?.[0] || post;
  const files = [];
  const add = (url, ext, kind, optional = false, alternateUrls = []) => {
    if (!url) return;
    try { url = permittedMedia(url).toString(); } catch { return; }
    if (files.some((item) => item.url === url)) return;
    const index = String(files.length + 1).padStart(3, "0");
    files.push({ name: "media/" + index + "." + (ext || suffix(url)), url, kind, optional,
      alternateUrls: alternateUrls.filter((candidate) => {
        try { permittedMedia(candidate); return true; } catch { return false; }
      }) });
  };
  if (source.is_gallery && source.gallery_data?.items?.length) {
    for (const item of source.gallery_data.items) {
      const media = source.media_metadata?.[item.media_id];
      if (!media) continue;
      const asset = media.s || {};
      const original = asset.u || asset.gif || asset.mp4;
      const ext = /png/.test(media.m || "") ? "png" : /webp/.test(media.m || "") ? "webp"
        : /gif/.test(media.m || "") ? "gif" : suffix(decodeEntities(original), "jpg");
      add(original, ext, "gallery");
      if (asset.mp4 && asset.mp4 !== original) add(asset.mp4, "mp4", "gallery-animation", true);
    }
  }
  const video = source.secure_media?.reddit_video || source.media?.reddit_video;
  if (video?.fallback_url) {
    add(video.fallback_url, "mp4", "video");
    // Reddit may use signed packaged-media URLs for the video while its DASH
    // manifest still points to v.redd.it. Derive audio from the manifest host.
    const origin = new URL(decodeEntities(video.dash_url || video.fallback_url));
    if (origin.hostname === "v.redd.it") {
      const base = origin.origin + origin.pathname.replace(/\/[^/]+$/, "");
      add(video.dash_url || base + "/DASHPlaylist.mpd", "mpd", "video-manifest", true);
      add(base + "/CMAF_AUDIO_128.mp4", "mp4", "video-audio", true, [
        base + "/CMAF_AUDIO_64.mp4", base + "/DASH_AUDIO_128.mp4",
        base + "/DASH_AUDIO_64.mp4", base + "/DASH_audio.mp4", base + "/audio",
      ]);
    }
  } else if (!source.is_gallery) {
    const mediaUrl = source.url_overridden_by_dest || source.url;
    if (mediaUrl) {
      try {
        const checked = permittedMedia(mediaUrl);
        if (/^i\.(?:redd\.it|imgur\.com)$/.test(checked.hostname)) add(checked.toString(), suffix(checked.toString(), "jpg"), "image");
        else if (checked.hostname === "v.redd.it") add(checked.toString(), suffix(checked.toString(), "mp4"), "video");
      } catch { /* External links are preserved in post.json; they are not fetched. */ }
    }
    if (!files.length && source.preview?.images?.[0]?.source?.url && !source.is_self) {
      add(source.preview.images[0].source.url, "jpg", "preview", true);
    }
  }
  return files;
}
export async function planPost(input, env = {}) {
  const id = await resolveRedditId(input);
  const post = await fetchRedditPost(id, env);
  const source = post.crosspost_parent_list?.[0] || post;
  const filename = "reddit-" + id + "-" + safeSlug(post.title) + ".tar.zst";
  return {
    id, filename, folder: "reddit-" + id,
    post: {
      archivedAt: new Date().toISOString(),
      permalink: "https://www.reddit.com" + post.permalink,
      title: post.title, author: post.author, subreddit: post.subreddit,
      createdUtc: post.created_utc, selftext: post.selftext || "",
      sourceUrl: "https://www.reddit.com/comments/" + id + "/",
      crosspostSource: source.id !== post.id ? "https://www.reddit.com/comments/" + source.id + "/" : null,
      externalUrl: source.url_overridden_by_dest || source.url || null,
      originalData: post,
    },
    files: planMedia(post),
  };
}
export async function proxyMedia(input) {
  let current = permittedMedia(input).toString();
  for (let i = 0; i < 4; i++) {
    const response = await fetch(current, {
      redirect: "manual",
      headers: { "accept": "*/*", "user-agent": "web:gooning.party-archiver:1.0 (private personal archiver)" },
    });
    if (response.status >= 300 && response.status < 400) {
      const target = response.headers.get("location");
      if (!target) throw new ArchiveError("Media redirected without a location.", 502);
      current = permittedMedia(new URL(target, current).toString()).toString();
      continue;
    }
    if (!response.ok || !response.body) throw new ArchiveError("Media download failed (HTTP " + response.status + ").", 502);
    const size = Number(response.headers.get("content-length") || 0);
    if (size > MAX_MEDIA_BYTES) throw new ArchiveError("This media file exceeds the 512 MiB download limit.", 413);
    if (/text\/html/i.test(response.headers.get("content-type") || "")) throw new ArchiveError("Media server returned an HTML page instead of a file.", 502);
    return new Response(response.body, { status: 200, headers: {
      "content-type": "application/octet-stream",
      ...(size > 0 ? { "content-length": String(size) } : {}),
      "cache-control": "private, no-store",
      "x-gparty-api": "1",
    } });
  }
  throw new ArchiveError("Too many media redirects.", 502);
}
