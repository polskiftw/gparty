import viewer from "./viewer.js";
import { FAVICON_SVG, ICON_ASSETS, SITE_MANIFEST } from "./icons.js";

const ROBOTS_POLICY = "noindex, nofollow, noarchive, nosnippet, noimageindex";
const PERMISSIONS_POLICY = [
  "accelerometer=()",
  "camera=()",
  "geolocation=()",
  "gyroscope=()",
  "microphone=()",
  "payment=()",
  "usb=()",
].join(", ");
const CONTENT_SECURITY_POLICY = [
  "default-src 'none'",
  "script-src 'self'",
  "style-src 'self'",
  "img-src 'self' data: blob:",
  "media-src 'self' blob:",
  "connect-src 'self'",
  "font-src 'self'",
  "object-src 'none'",
  "base-uri 'none'",
  "form-action 'self'",
  "frame-ancestors 'none'",
  "manifest-src 'self'",
].join("; ");
const ICON_LINKS = [
  '<link rel="icon" href="/favicon.ico" sizes="any">',
  '<link rel="icon" href="/favicon.svg" type="image/svg+xml">',
  '<link rel="icon" href="/favicon-32x32.png" type="image/png" sizes="32x32">',
  '<link rel="icon" href="/favicon-16x16.png" type="image/png" sizes="16x16">',
  '<link rel="apple-touch-icon" href="/apple-touch-icon.png">',
  '<link rel="manifest" href="/site.webmanifest">',
].join("\n  ");

function decodeBase64(value) {
  const decoded = atob(value);
  const bytes = new Uint8Array(decoded.length);
  for (let index = 0; index < decoded.length; index += 1) {
    bytes[index] = decoded.charCodeAt(index);
  }
  return bytes;
}

function iconResponse(pathname) {
  const asset = ICON_ASSETS[pathname];
  if (asset) {
    return new Response(decodeBase64(asset.base64), {
      headers: {
        "content-type": asset.type,
        "cache-control": "public, max-age=31536000, immutable",
      },
    });
  }
  if (pathname === "/favicon.svg") {
    return new Response(FAVICON_SVG, {
      headers: {
        "content-type": "image/svg+xml; charset=utf-8",
        "cache-control": "public, max-age=31536000, immutable",
      },
    });
  }
  if (pathname === "/site.webmanifest") {
    return new Response(SITE_MANIFEST, {
      headers: {
        "content-type": "application/manifest+json; charset=utf-8",
        "cache-control": "public, max-age=3600",
      },
    });
  }
  return null;
}

function withIconLinks(response) {
  const contentType = response.headers.get("content-type") || "";
  if (!contentType.startsWith("text/html")) return response;
  return response.text().then((html) => {
    const decorated = html.includes(ICON_LINKS)
      ? html
      : html.replace("<title>GParty</title>", `<title>GParty</title>\n  ${ICON_LINKS}`);
    return new Response(decorated, {
      status: response.status,
      statusText: response.statusText,
      headers: response.headers,
    });
  });
}

export default {
  async fetch(request, env, ctx) {
    const pathname = new URL(request.url).pathname;
    let response;
    try {
      response = iconResponse(pathname) || await viewer.fetch(request, env, ctx);
      if (pathname === "/" && response.ok) response = await withIconLinks(response);
    } catch (problem) {
      console.error("Viewer request failed", problem);
      response =
        pathname === "/api/random" || pathname === "/api/tags" || pathname === "/api/sources"
          ? new Response(
              JSON.stringify({ error: "Random media is temporarily unavailable." }),
              {
                status: 503,
                headers: {
                  "content-type": "application/json; charset=utf-8",
                  "cache-control": "no-store",
                  "retry-after": "1",
                },
              },
            )
          : new Response("Service temporarily unavailable", { status: 503 });
    }
    const headers = new Headers(response.headers);

    headers.set("x-robots-tag", ROBOTS_POLICY);
    headers.set("permissions-policy", PERMISSIONS_POLICY);
    headers.set("content-security-policy", CONTENT_SECURITY_POLICY);
    headers.set("x-content-type-options", "nosniff");
    headers.set("referrer-policy", "no-referrer");

    return new Response(response.body, {
      status: response.status,
      statusText: response.statusText,
      headers,
    });
  },
};
