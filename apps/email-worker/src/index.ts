import { EmailMessage } from "cloudflare:email";
import PostalMime, {
  type Address,
  type Attachment,
  type Email,
  type Mailbox,
} from "postal-mime";

interface Env {
  EMAIL: SendEmail;
  EMAIL_SECRET_SUFFIX: string;
  FORWARD_TO: string;
  OUTBOUND_FROM: string;
}

const DOMAIN = "gooning.party";
const FIXED_ADDRESSES = new Set(["abuse", "dmca"]);
const MAX_MESSAGE_BYTES = 25 * 1024 * 1024;

export default {
  async email(message: ForwardableEmailMessage, env: Env): Promise<void> {
    validateConfiguration(env);

    if (message.rawSize > MAX_MESSAGE_BYTES) {
      message.setReject("Message exceeds the 25 MiB email limit.");
      return;
    }

    const incoming = parseAddress(message.to);
    if (!incoming || incoming.domain !== DOMAIN) {
      message.setReject("This address is not accepted.");
      return;
    }

    const cleanLocalPart = cleanRecipient(incoming.localPart, env.EMAIL_SECRET_SUFFIX);
    if (!cleanLocalPart) {
      message.setReject("This address does not exist.");
      return;
    }

    const originalRecipient = `${incoming.localPart}@${DOMAIN}`;
    const cleanRecipientAddress = `${cleanLocalPart}@${DOMAIN}`;
    const parsed = await PostalMime.parse(message.raw);
    const raw = buildSanitizedMessage({
      parsed,
      originalRecipient,
      cleanRecipient: cleanRecipientAddress,
      outboundFrom: env.OUTBOUND_FROM,
    });

    const outgoing = new EmailMessage(env.OUTBOUND_FROM, env.FORWARD_TO, raw);
    await env.EMAIL.send(outgoing);
  },
} satisfies ExportedHandler<Env>;

function validateConfiguration(env: Env): void {
  if (!env.EMAIL_SECRET_SUFFIX) {
    throw new Error("EMAIL_SECRET_SUFFIX is missing or empty.");
  }
  if (!isEmail(env.FORWARD_TO)) {
    throw new Error("FORWARD_TO must be a valid verified destination address.");
  }
  if (!isEmail(env.OUTBOUND_FROM) || !env.OUTBOUND_FROM.toLowerCase().endsWith(`@${DOMAIN}`)) {
    throw new Error(`OUTBOUND_FROM must be an address at ${DOMAIN}.`);
  }
}

function parseAddress(address: string): { localPart: string; domain: string } | null {
  const at = address.lastIndexOf("@");
  if (at <= 0 || at === address.length - 1) return null;
  return {
    localPart: address.slice(0, at),
    domain: address.slice(at + 1).toLowerCase(),
  };
}

function cleanRecipient(localPart: string, secretSuffix: string): string | null {
  const lowered = localPart.toLowerCase();
  if (FIXED_ADDRESSES.has(lowered)) return lowered;
  if (!localPart.endsWith(secretSuffix)) return null;

  const clean = localPart.slice(0, -secretSuffix.length);
  if (!clean || clean.endsWith(".") || clean.startsWith(".")) return null;
  if (!/^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+$/.test(clean)) return null;
  return clean;
}

function buildSanitizedMessage(options: {
  parsed: Email;
  originalRecipient: string;
  cleanRecipient: string;
  outboundFrom: string;
}): string {
  const { parsed, originalRecipient, cleanRecipient, outboundFrom } = options;
  const boundaryMixed = randomBoundary("mixed");
  const boundaryAlternative = randomBoundary("alternative");
  const sender = getMailbox(parsed.from);
  const senderAddress = sender?.address || "unknown-sender@invalid.example";
  const senderName = sender?.name.trim() || senderAddress;
  const displayName = `${senderName} via gooning.party`;

  const subject = scrub(parsed.subject || "(no subject)", originalRecipient, cleanRecipient);
  const text = scrub(parsed.text || htmlToFallbackText(parsed.html || ""), originalRecipient, cleanRecipient);
  const html = parsed.html
    ? scrub(parsed.html, originalRecipient, cleanRecipient)
    : `<pre style="white-space:pre-wrap">${escapeHtml(text)}</pre>`;

  const headers = [
    `From: ${encodeDisplayName(displayName)} <${outboundFrom}>`,
    `To: <${cleanRecipient}>`,
    `Reply-To: ${formatMailbox(sender?.name || "", senderAddress)}`,
    `Subject: ${encodeHeader(subject)}`,
    `Date: ${formatDate(parsed.date)}`,
    `Message-ID: <${crypto.randomUUID()}@${DOMAIN}>`,
    "MIME-Version: 1.0",
    `X-GParty-Original-Sender: ${sanitizeHeaderValue(senderAddress)}`,
  ];

  if (parsed.messageId) headers.push(`X-GParty-Original-Message-ID: ${sanitizeHeaderValue(parsed.messageId)}`);
  if (parsed.inReplyTo) headers.push(`In-Reply-To: ${sanitizeHeaderValue(parsed.inReplyTo)}`);
  if (parsed.references) headers.push(`References: ${sanitizeHeaderValue(parsed.references)}`);

  const attachments = parsed.attachments || [];
  if (attachments.length === 0) {
    headers.push(`Content-Type: multipart/alternative; boundary="${boundaryAlternative}"`);
    return `${headers.join("\r\n")}\r\n\r\n${buildAlternative(text, html, boundaryAlternative)}`;
  }

  headers.push(`Content-Type: multipart/mixed; boundary="${boundaryMixed}"`);
  const parts = [
    `--${boundaryMixed}\r\nContent-Type: multipart/alternative; boundary="${boundaryAlternative}"\r\n\r\n${buildAlternative(text, html, boundaryAlternative)}`,
    ...attachments.map((attachment, index) => buildAttachment(attachment, boundaryMixed, index)),
    `--${boundaryMixed}--\r\n`,
  ];

  return `${headers.join("\r\n")}\r\n\r\n${parts.join("\r\n")}`;
}

function getMailbox(address?: Address): Mailbox | undefined {
  if (!address || ("group" in address && address.group !== undefined)) return undefined;
  return address;
}

function buildAlternative(text: string, html: string, boundary: string): string {
  return [
    `--${boundary}`,
    "Content-Type: text/plain; charset=UTF-8",
    "Content-Transfer-Encoding: base64",
    "",
    wrapBase64(bytesToBase64(new TextEncoder().encode(text))),
    `--${boundary}`,
    "Content-Type: text/html; charset=UTF-8",
    "Content-Transfer-Encoding: base64",
    "",
    wrapBase64(bytesToBase64(new TextEncoder().encode(html))),
    `--${boundary}--`,
    "",
  ].join("\r\n");
}

function buildAttachment(attachment: Attachment, boundary: string, index: number): string {
  const fallbackName = `attachment-${index + 1}`;
  const filename = sanitizeFilename(attachment.filename || fallbackName);
  const disposition = attachment.disposition === "inline" || attachment.related ? "inline" : "attachment";
  const lines = [
    `--${boundary}`,
    `Content-Type: ${sanitizeMimeType(attachment.mimeType)}; name="${filename}"`,
    "Content-Transfer-Encoding: base64",
    `Content-Disposition: ${disposition}; filename="${filename}"`,
  ];

  if (attachment.contentId) {
    lines.push(`Content-ID: <${attachment.contentId.replace(/[<>\r\n]/g, "")}>`);
  }

  lines.push("", wrapBase64(attachmentToBase64(attachment)), "");
  return lines.join("\r\n");
}

function attachmentToBase64(attachment: Attachment): string {
  if (typeof attachment.content === "string") {
    return attachment.encoding === "base64"
      ? attachment.content.replace(/\s+/g, "")
      : bytesToBase64(new TextEncoder().encode(attachment.content));
  }
  const bytes = attachment.content instanceof Uint8Array
    ? attachment.content
    : new Uint8Array(attachment.content);
  return bytesToBase64(bytes);
}

function scrub(value: string, originalAddress: string, cleanAddress: string): string {
  return value.replace(new RegExp(escapeRegExp(originalAddress), "gi"), cleanAddress);
}

function formatMailbox(name: string, address: string): string {
  return name.trim() ? `${encodeDisplayName(name)} <${address}>` : `<${address}>`;
}

function encodeDisplayName(value: string): string {
  const clean = sanitizeHeaderValue(value);
  return /^[\x20-\x7E]+$/.test(clean)
    ? `"${clean.replace(/["\\]/g, "\\$&")}"`
    : encodeHeader(clean);
}

function encodeHeader(value: string): string {
  const clean = sanitizeHeaderValue(value);
  if (/^[\x20-\x7E]*$/.test(clean)) return clean;
  return `=?UTF-8?B?${bytesToBase64(new TextEncoder().encode(clean))}?=`;
}

function sanitizeHeaderValue(value: string): string {
  return value.replace(/[\r\n]+/g, " ").trim();
}

function sanitizeFilename(value: string): string {
  return value.replace(/["\\\r\n]/g, "_").slice(0, 180) || "attachment";
}

function sanitizeMimeType(value: string): string {
  return /^[A-Za-z0-9!#$&^_.+-]+\/[A-Za-z0-9!#$&^_.+-]+$/.test(value)
    ? value
    : "application/octet-stream";
}

function formatDate(value?: string): string {
  const date = value ? new Date(value) : new Date();
  return Number.isNaN(date.getTime()) ? new Date().toUTCString() : date.toUTCString();
}

function randomBoundary(label: string): string {
  return `gparty-${label}-${crypto.randomUUID()}`;
}

function bytesToBase64(bytes: Uint8Array): string {
  let binary = "";
  const chunkSize = 0x8000;
  for (let offset = 0; offset < bytes.length; offset += chunkSize) {
    binary += String.fromCharCode(...bytes.subarray(offset, offset + chunkSize));
  }
  return btoa(binary);
}

function wrapBase64(value: string): string {
  return value.match(/.{1,76}/g)?.join("\r\n") || "";
}

function htmlToFallbackText(html: string): string {
  return html
    .replace(/<style[\s\S]*?<\/style>/gi, "")
    .replace(/<script[\s\S]*?<\/script>/gi, "")
    .replace(/<br\s*\/?>/gi, "\n")
    .replace(/<\/p>/gi, "\n\n")
    .replace(/<[^>]+>/g, "")
    .replace(/&nbsp;/gi, " ")
    .replace(/&amp;/gi, "&")
    .replace(/&lt;/gi, "<")
    .replace(/&gt;/gi, ">");
}

function escapeHtml(value: string): string {
  return value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function isEmail(value: string): boolean {
  return /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(value);
}
