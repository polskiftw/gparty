import privateDropbox, { authenticated } from "../../private-dropbox/src/worker.js";

const CONTENT_SECURITY_POLICY = [
  "default-src 'none'",
  "script-src 'self'",
  "style-src 'self'",
  "img-src 'self' data:",
  "connect-src 'self'",
  "font-src 'self'",
  "media-src 'self'",
  "object-src 'none'",
  "base-uri 'none'",
  "frame-ancestors 'none'",
  "form-action 'self'",
].join("; ");
const PERMISSIONS_POLICY = "camera=(), microphone=(), geolocation=(), payment=(), usb=()";

/**
 * gooning.party is now the private shared file drop, not a media gallery.
 * Every response, including the UI and file bodies, requires verified mTLS.
 */
export default {
  async fetch(request, env, ctx) {
    let response;
    if (!authenticated(request)) {
      response = new Response("A verified client certificate is required.", { status: 403 });
    } else {
      try {
        response = await privateDropbox.fetch(request, env, ctx);
      } catch (error) {
        console.error("Private file drop failed", error?.name || "unknown");
        response = new Response("Service temporarily unavailable", { status: 503 });
      }
    }
    const headers = new Headers(response.headers);
    headers.set("content-security-policy", CONTENT_SECURITY_POLICY);
    headers.set("x-content-type-options", "nosniff");
    headers.set("x-robots-tag", "noindex, nofollow, noarchive, nosnippet, noimageindex");
    headers.set("referrer-policy", "no-referrer");
    headers.set("permissions-policy", PERMISSIONS_POLICY);
    headers.set("cache-control", "private, no-store");
    return new Response(response.body, {
      status: response.status, statusText: response.statusText, headers,
    });
  },
};
