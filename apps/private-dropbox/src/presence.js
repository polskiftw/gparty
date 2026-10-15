// Shared, genuinely live hit counter. Only the mTLS-protected Worker can call this object.
// Short-lived random tab IDs; no IP addresses, cookies, user agents, or names are recorded.
const PRESENCE_TTL_MS = 60000;
export class VisitorCounter {
  constructor(state) { this.storage = state.storage; }
  async fetch(request) {
    if (request.method !== "POST") return Response.json({error:"Method not allowed."},{status:405});
    const payload = await request.json().catch(() => null);
    if (!payload || !/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(payload.sessionId || "")
      || !["hello","beat"].includes(payload.action)) {
      return Response.json({error:"Invalid presence update."},{status:400});
    }
    const now = Date.now();
    const existing = await this.storage.list({prefix:"online:"});
    let online = 0;
    for (const [key, until] of existing) {
      if (until <= now) await this.storage.delete(key);
      else online++;
    }
    const key = "online:" + payload.sessionId;
    const known = (existing.get(key) || 0) > now;
    let visits = (await this.storage.get("total-visits")) || 0;
    if (!known) {
      visits++;
      online++;
      await this.storage.put("total-visits", visits);
    }
    await this.storage.put(key, now + PRESENCE_TTL_MS);
    return Response.json({visits, online, ttlSeconds:PRESENCE_TTL_MS / 1000}, {
      headers:{"cache-control":"private, no-store","x-gparty-api":"1"},
    });
  }
}
