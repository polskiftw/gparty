import viewer from "./viewer.js";

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
  "manifest-src 'none'",
].join("; ");

export default {
  async fetch(request, env, ctx) {
    let response;
    try {
      response = await viewer.fetch(request, env, ctx);
    } catch (problem) {
      console.error("Viewer request failed", problem);
      const pathname = new URL(request.url).pathname;
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
