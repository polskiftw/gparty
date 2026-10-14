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
    const isApi = new URL(request.url).pathname.startsWith("/_/api/");
    const failure = (message, code, errorId) => isApi
      ? new Response(JSON.stringify({
          error: message, ...(errorId ? { errorId } : {}),
        }), {
          status: code, headers: {
            "content-type": "application/json; charset=utf-8",
            "x-gparty-api": "1",
          },
        })
      : new Response(message, { status: code });
    let response;
    if (!authenticated(request)) {
      response = failure("A verified client certificate is required.", 403);
    } else {
      try {
        response = await privateDropbox.fetch(request, env, ctx);
      } catch (error) {
        const errorId = crypto.randomUUID();
        console.error("GParty outer Worker failure", {
          errorId, name: error?.name || "Error", message: error?.message || "unknown",
        });
        response = failure("Service temporarily unavailable. Check Cloudflare Worker logs.", 503, errorId);
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
