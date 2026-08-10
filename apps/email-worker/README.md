# GParty Email Worker

This Worker accepts secret-suffixed addresses at `gooning.party`, removes the suffix, rebuilds the message, and delivers the sanitized copy to a verified Gmail destination.

## Address behavior

- `anything<SECRET>@gooning.party` is accepted and appears in Gmail as `anything@gooning.party`.
- Bare addresses such as `anything@gooning.party` are rejected.
- `abuse@gooning.party` and `dmca@gooning.party` are accepted without the suffix.
- The original secret-bearing recipient is not copied into the rebuilt message headers.
- Exact appearances of the secret-bearing address in the subject, text body, or HTML body are replaced with the clean address.

## Private Cloudflare secrets

Create these encrypted Worker secrets in Cloudflare. Never commit their values to GitHub.

- `EMAIL_SECRET_SUFFIX` — the private suffix.
- `FORWARD_TO` — the verified Gmail destination address.
- `OUTBOUND_FROM` — the relay envelope sender at `gooning.party`, such as `relay@gooning.party`.

The public `send_email` binding is named `EMAIL` but contains no destination address. Cloudflare permits an unrestricted binding to send only to destination addresses already verified in the account. The Worker reads the selected Gmail destination from the encrypted `FORWARD_TO` secret at runtime.

## One-time setup

1. In Cloudflare, open **Email > Email Routing** for `gooning.party`.
2. Add the Gmail address as a destination and complete its verification email.
3. Enable Email Routing and allow Cloudflare to create the required MX and SPF DNS records.
4. Deploy this Worker from the `apps/email-worker` directory.
5. Add the three encrypted secrets in the Worker's Cloudflare settings, or with Wrangler:

```bash
npx wrangler secret put EMAIL_SECRET_SUFFIX
npx wrangler secret put FORWARD_TO
npx wrangler secret put OUTBOUND_FROM
```

6. Open **Email > Email Routing > Routing rules**.
7. Set the catch-all action to **Send to a Worker** and choose `gparty-email-1`.
8. Do not create ordinary forwarding rules for the secret addresses. The catch-all Worker handles them.

## Deploy

```bash
cd apps/email-worker
npm install
npm run check
npm run deploy
```

## Test

With your private suffix configured:

- Send to `test<SECRET>@gooning.party`. Gmail should receive it with visible `To: test@gooning.party`.
- Send to `test@gooning.party`. It should be rejected.
- Send to `dmca@gooning.party` and `abuse@gooning.party`. Both should arrive without a suffix.
- Reply in Gmail. The reply should go to the original sender because the Worker sets `Reply-To` to that sender.

## Message handling

The Worker parses and reconstructs MIME rather than using `message.forward()`. That allows the visible `To` header to contain the clean alias while the SMTP envelope delivers to Gmail. Text, HTML, ordinary attachments, and inline attachments are preserved. The verified Gmail destination allows messages up to Cloudflare's 25 MiB verified-destination limit.
