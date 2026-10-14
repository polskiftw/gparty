// Multipart file uploads through the mTLS-protected Worker.
// No presigned R2 URL is ever exposed: every part passes the same certificate check.
export const MiB = 1024 * 1024;
export const SINGLE_UPLOAD_BYTES = 64 * MiB;
export const MAX_PART_BYTES = 64 * MiB;
export const MAX_PARTS = 10000;
export const MAX_FILE_BYTES = MAX_PART_BYTES * MAX_PARTS;
const SESSIONS = "multipart-sessions/";

export function partSizeFor(size) {
  if (!Number.isSafeInteger(size) || size <= SINGLE_UPLOAD_BYTES || size > MAX_FILE_BYTES) {
    throw new Error("File size is outside the supported multipart range.");
  }
  // Each request stays safely below a 100 MB Cloudflare zone upload limit.
  const quantum = 8 * MiB;
  const sizePerPart = Math.max(16 * MiB, Math.ceil(size / MAX_PARTS / quantum) * quantum);
  if (sizePerPart > MAX_PART_BYTES || Math.ceil(size / sizePerPart) > MAX_PARTS) {
    throw new Error("Too many upload parts.");
  }
  return sizePerPart;
}
function response(value, code = 200) {
  return new Response(JSON.stringify(value), {
    status: code,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "private, no-store" },
  });
}
function problem(text, code) { return response({ error: text }, code); }
function storageKey(id) { return SESSIONS + encodeURIComponent(id); }
function safeId(id) {
  return typeof id === "string" && id.length >= 10 && id.length < 500 && !/[\u0000-\u001f]/u.test(id);
}
async function readSession(bucket, id, path) {
  if (!safeId(id)) return null;
  const object = await bucket.get(storageKey(id));
  if (!object) return null;
  const session = JSON.parse(await object.text());
  if (session.path !== path || session.uploadId !== id) return null;
  return session;
}

export async function startMultipart(bucket, path, fileSize) {
  let partSize;
  try { partSize = partSizeFor(fileSize); }
  catch (error) { return problem(error.message, 413); }

  if (await bucket.head("files/" + path)) return problem("File already exists.", 409);
  const upload = await bucket.createMultipartUpload("files/" + path, {
    httpMetadata: { contentType: "application/octet-stream", cacheControl: "private, no-store" },
    customMetadata: { originalName: path.split("/").at(-1) },
  });
  const state = {
    path, uploadId: upload.uploadId, size: fileSize, partSize,
    partCount: Math.ceil(fileSize / partSize), createdAt: Date.now(),
  };
  try {
    await bucket.put(storageKey(upload.uploadId), JSON.stringify(state), {
      httpMetadata: { contentType: "application/json", cacheControl: "private, no-store" },
    });
  } catch (error) {
    await upload.abort().catch(() => {});
    throw error;
  }
  return response({ uploadId: state.uploadId, partSize, partCount: state.partCount }, 201);
}

export async function uploadMultipartPart(bucket, request, path, uploadId, partNumber) {
  const state = await readSession(bucket, uploadId, path);
  if (!state) return problem("Unknown or expired multipart session.", 404);
  if (!Number.isSafeInteger(partNumber) || partNumber < 1 || partNumber > state.partCount) {
    return problem("Invalid part number.", 400);
  }
  const expected = partNumber === state.partCount
    ? state.size - state.partSize * (partNumber - 1) : state.partSize;
  const actualHeader = request.headers.get("content-length");
  const actual = actualHeader === null ? NaN : Number(actualHeader);
  if (!Number.isSafeInteger(actual) || actual !== expected || !request.body) {
    return problem("Part length does not match its expected size.", 400);
  }
  try {
    const upload = bucket.resumeMultipartUpload("files/" + path, uploadId);
    const part = await upload.uploadPart(partNumber, request.body);
    return response({ partNumber: part.partNumber, etag: part.etag });
  } catch (cause) {
    if (/NoSuchUpload|does not exist|not found/i.test(String(cause?.message || ""))) {
      return problem("Unknown or expired multipart session.", 404);
    }
    return problem("Part upload failed; it may be retried.", 503);
  }
}

export async function finishMultipart(bucket, path, uploadId, parts) {
  const state = await readSession(bucket, uploadId, path);
  if (!state) return problem("Unknown or expired multipart session.", 404);
  if (!Array.isArray(parts) || parts.length !== state.partCount ||
    parts.some((part, idx) => part?.partNumber !== idx + 1 ||
      typeof part.etag !== "string" || !/^[a-fA-F0-9]{32}$/.test(part.etag))) {
    return problem("Missing or invalid multipart part receipts.", 400);
  }
  if (await bucket.head("files/" + path)) {
    return problem("A file with this name appeared during the upload.", 409);
  }
  try {
    const upload = bucket.resumeMultipartUpload("files/" + path, uploadId);
    const file = await upload.complete(parts);
    if (file.size !== state.size) {
      // Preserve unexpected data for investigation instead of risking deletion
      // of somebody else's object after a concurrent write.
      return problem("Completed file size did not match. Inspect stored object.", 500);
    }
    await bucket.delete(storageKey(uploadId));
    return response({ uploaded: true, path, size: file.size });
  } catch {
    return problem("Multipart completion failed; retry or cancel the upload.", 503);
  }
}

export async function cancelMultipart(bucket, path, uploadId) {
  const state = await readSession(bucket, uploadId, path);
  if (!state) return problem("Unknown or expired multipart session.", 404);
  try {
    const upload = bucket.resumeMultipartUpload("files/" + path, uploadId);
    await upload.abort();
    await bucket.delete(storageKey(uploadId));
    return response({ aborted: true });
  } catch {
    return problem("Could not cancel multipart upload.", 503);
  }
}
