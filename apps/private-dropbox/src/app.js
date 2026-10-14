"use strict";
const $ = (id) => document.getElementById(id);
const directory = decodeURIComponent(location.pathname.replace(/^\/|\/$/g, ""));
const segments = directory ? directory.split("/") : [];
const text = (tag, value, cls) => {
  const element = document.createElement(tag);
  element.textContent = value;
  if (cls) element.className = cls;
  return element;
};
const encoded = (path) => path.split("/").map(encodeURIComponent).join("/");
const fullPath = (filename) => [...segments, filename].join("/");
const fileUrl = (filename) => "/" + encoded(fullPath(filename));
const folderUrl = (parts) => "/" + encoded(parts.join("/")) + (parts.length ? "/" : "");
let aiEnabled = false;
let maxFileBytes = 64 * 1024 * 1024 * 10000;
const SINGLE_UPLOAD_BYTES = 64 * 1024 * 1024;
let selectedShare = "";

function status(message, bad = false) {
  $("status").textContent = message;
  $("status").classList.toggle("error", bad);
}
async function requestJson(path, options = {}) {
  const response = await fetch(path, { cache: "no-store", ...options });
  const body = await response.json();
  if (!response.ok) throw new Error(body.error || "Request failed.");
  return body;
}
const writeHeaders = { "x-gparty-write": "1" };
function button(name, callback, cls) {
  const element = text("button", name);
  element.type = "button";
  if (cls) element.className = cls;
  element.addEventListener("click", callback);
  return element;
}
async function copyUrl(url) {
  try {
    await navigator.clipboard.writeText(url);
    status("LINK COPIED: " + url);
  } catch {
    window.prompt("Copy this link:", url);
  }
}
function renderBreadcrumbs() {
  const nav = $("crumbs");
  nav.replaceChildren();
  const home = text("a", "⌂ ROOT");
  home.href = "/";
  nav.append(home);
  segments.forEach((segment, index) => {
    nav.append(text("em", " / "));
    if (index === segments.length - 1) nav.append(text("span", segment));
    else {
      const link = text("a", segment);
      link.href = folderUrl(segments.slice(0, index + 1));
      nav.append(link);
    }
  });
}
function sizeLabel(bytes) {
  if (bytes < 1024) return bytes + " B";
  if (bytes < 1024 ** 2) return (bytes / 1024).toFixed(1) + " KiB";
  if (bytes < 1024 ** 3) return (bytes / 1024 ** 2).toFixed(2) + " MiB";
  return (bytes / 1024 ** 3).toFixed(2) + " GiB";
}
function addFolder(name) {
  const row = text("div", "", "file-row");
  row.append(text("span", "📁", "file-icon"));
  const meta = text("div", "", "file-meta");
  const link = text("a", name + "/", "file-title folder-link");
  link.href = folderUrl([...segments, name]);
  meta.append(link, text("div", "DIRECTORY", "file-sub"));
  const actions = text("div", "", "mini-actions");
  actions.append(button("COPY LINK", () => copyUrl(link.href), "secondary"));
  row.append(meta, actions);
  $("file-list").append(row);
}
function addFile(file) {
  const row = text("div", "", "file-row");
  row.append(text("span", file.text ? "📝" : "💾", "file-icon"));
  const meta = text("div", "", "file-meta");
  const link = text("a", file.name, "file-title");
  link.href = fileUrl(file.name);
  const info = text("div", sizeLabel(file.size) + (file.modified ? " · " + new Date(file.modified).toLocaleString() : "") + (file.ai ? " · ✓ AI READY" : ""), "file-sub");
  meta.append(link, info);
  const actions = text("div", "", "mini-actions");
  if (file.text) actions.append(button("PREVIEW", () => showPreview(file.name), "secondary"));
  actions.append(button("COPY LINK", () => copyUrl(link.href), "secondary"));
  if (file.text) {
    const share = button(file.ai ? "↻ RE-SYNC AI" : "SHARE WITH AI", () => confirmShare(file.name), "ai");
    share.disabled = !aiEnabled;
    share.title = !aiEnabled ? "Private GitHub bridge is not configured yet"
      : "Export text to private GitHub in individually readable chunks";
    actions.append(share);
  }
  row.append(meta, actions);
  $("file-list").append(row);
}
async function load() {
  renderBreadcrumbs();
  try {
    const [config, data] = await Promise.all([
      requestJson("/_/api/config"),
      requestJson("/_/api/list?path=" + encodeURIComponent(directory)),
    ]);
    aiEnabled = config.aiEnabled;
    maxFileBytes = config.maxFileBytes || maxFileBytes;
    $("file-list").replaceChildren();
    data.folders.forEach(addFolder);
    data.files.forEach(addFile);
    $("count").textContent = (data.folders.length + data.files.length) + " items";
    if (!data.files.length && !data.folders.length) {
      $("file-list").append(text("p", "NOTHING HERE YET. DROP SOMETHING TERRIBLE.", "empty"));
    }
    if (data.truncated) status("This folder has more than 1000 entries. Not all are shown.", true);
    else if (!aiEnabled) status("FILE DROP IS READY. AI SHARING WILL APPEAR WHEN THE PRIVATE GITHUB BRIDGE IS CONFIGURED.");
    else status("READY FOR YOUR NEXT CRASH REPORT.");
  } catch (problem) {
    status("Failed to load: " + problem.message, true);
  }
}
async function uploadOne(file, setProgress) {
  const path = fullPath(file.name);
  if (file.size <= SINGLE_UPLOAD_BYTES) {
    await requestJson("/_/api/upload?path=" + encodeURIComponent(path), {
      method: "POST", headers: writeHeaders, body: file,
    });
    setProgress(file.size, file.size);
    return;
  }
  const opened = await requestJson("/_/api/mpu/create", {
    method: "POST", headers: { ...writeHeaders, "content-type": "application/json" },
    body: JSON.stringify({ path, size: file.size }),
  });
  const { uploadId, partSize, partCount } = opened;
  const receipts = Array(partCount);
  let nextPart = 0;
  let uploaded = 0;
  try {
    const send = async () => {
      while (nextPart < partCount) {
        const index = nextPart++;
        const offset = index * partSize;
        const blob = file.slice(offset, Math.min(file.size, offset + partSize));
        const api = "/_/api/mpu/part?path=" + encodeURIComponent(path)
          + "&uploadId=" + encodeURIComponent(uploadId) + "&part=" + (index + 1);
        for (let attempt = 0; attempt < 4; attempt++) {
          try {
            const response = await requestJson(api, {
              method: "PUT", headers: writeHeaders, body: blob,
            });
            receipts[index] = response;
            uploaded += blob.size;
            setProgress(uploaded, file.size);
            break;
          } catch (cause) {
            if (attempt === 3) throw cause;
            await new Promise((resolve) => setTimeout(resolve, 400 * (attempt + 1)));
          }
        }
      }
    };
    await Promise.all(Array.from({ length: Math.min(3, partCount) }, send));
    await requestJson("/_/api/mpu/complete", {
      method: "POST", headers: { ...writeHeaders, "content-type": "application/json" },
      body: JSON.stringify({ path, uploadId, parts: receipts }),
    });
    setProgress(file.size, file.size);
  } catch (cause) {
    try {
      await requestJson("/_/api/mpu/cancel", {
        method: "DELETE", headers: { ...writeHeaders, "content-type": "application/json" },
        body: JSON.stringify({ path, uploadId }),
      });
    } catch { /* R2 automatically expires abandoned multipart uploads. */ }
    throw cause;
  }
}
async function uploadFiles(selected) {
  const files = [...selected];
  if (!files.length) return;
  let successful = 0;
  const failures = [];
  for (let index = 0; index < files.length; index++) {
    const file = files[index];
    try {
      if (file.size > maxFileBytes) throw new Error("File is above current multipart capacity.");
      await uploadOne(file, (done, total) => {
        const percent = total === 0 ? 100 : Math.floor(done / total * 100);
        status("UPLOADING " + (index + 1) + "/" + files.length + ": " + file.name + " · "
          + percent + "% (" + sizeLabel(done) + "/" + sizeLabel(total) + ")");
      });
      successful++;
    } catch (problem) { failures.push(file.name + ": " + problem.message); }
  }
  await load();
  if (failures.length) status(successful + " uploaded. ERRORS: " + failures.join(" · "), true);
  else status("SUCCESS! " + successful + " FILE(S) UPLOADED TO " + (directory ? "/" + directory + "/" : "/"));
}
async function newFolder() {
  const name = window.prompt("New folder name (in the current directory):");
  if (name === null) return;
  if (!name.trim() || name.includes("/") || name.includes("\\") || name === "." || name === "..") {
    status("That is not a valid folder name.", true);
    return;
  }
  try {
    const result = await requestJson("/_/api/folder", {
      method: "POST", headers: { ...writeHeaders, "content-type": "application/json" },
      body: JSON.stringify({ path: fullPath(name) }),
    });
    await load();
    status("CREATED FOLDER: /" + result.path + "/");
  } catch (problem) { status(problem.message, true); }
}
async function showPreview(name) {
  $("preview-title").textContent = name;
  $("preview-info").textContent = "Loading...";
  $("preview-body").replaceChildren();
  $("preview-dialog").showModal();
  try {
    const preview = await requestJson("/_/api/preview?path=" + encodeURIComponent(fullPath(name)));
    const lines = preview.text.split("\n").slice(0, 5000);
    const fragment = document.createDocumentFragment();
    lines.forEach((line, i) => {
      const row = text("div", "", "preview-line");
      row.append(text("span", String(i + 1), "preview-number"), text("span", line || " ", "preview-text"));
      fragment.append(row);
    });
    $("preview-body").append(fragment);
    $("preview-info").textContent = "READ-ONLY · " + sizeLabel(preview.size)
      + (preview.truncated || preview.text.split("\n").length > 5000 ? " · PREVIEW TRUNCATED AT 128 KiB / 5000 LINES" : "");
  } catch (problem) { $("preview-info").textContent = problem.message; }
}
function confirmShare(name) {
  selectedShare = fullPath(name);
  $("share-filename").textContent = "/" + selectedShare;
  $("share-dialog").showModal();
}
async function doShare() {
  $("share-dialog").close();
  const path = selectedShare;
  let part = 0;
  let etag = "";
  const receipts = [];
  $("share-confirm").disabled = true;
  try {
    let result;
    do {
      status("EXPORTING /" + path + " TO PRIVATE GITHUB · PIECE " + (part + 1) + "...");
      result = await requestJson("/_/api/share", {
        method: "POST", headers: { ...writeHeaders, "content-type": "application/json" },
        body: JSON.stringify({ path, part, etag }),
      });
      etag = result.etag;
      receipts.push(result.blobSha);
      part = result.nextPart;
      status("PRIVATE GITHUB: " + part + "/" + result.totalParts + " CHUNKS STORED");
    } while (part < result.totalParts);
    status("FINALIZING ONE PRIVATE GITHUB COMMIT FOR " + receipts.length + " CHUNK(S)...");
    const published = await requestJson("/_/api/share/finish", {
      method: "POST", headers: { ...writeHeaders, "content-type": "application/json" },
      body: JSON.stringify({ path, etag, parts: receipts }),
    });
    await load();
    status("AI-READY! GitHub file/index: " + published.githubPath
      + " · Tell ChatGPT to inspect this path in your private debug repository.");
  } catch (problem) {
    status("AI sync incomplete: " + problem.message
      + ". No complete AI-ready copy has been published; retry the export.", true);
  } finally {
    $("share-confirm").disabled = false;
  }
}
$("choose-files").addEventListener("click", () => $("file-picker").click());
$("drop-zone").addEventListener("click", () => $("file-picker").click());
$("drop-zone").addEventListener("keydown", (event) => {
  if (event.key === "Enter" || event.key === " ") { event.preventDefault(); $("file-picker").click(); }
});
$("file-picker").addEventListener("change", (event) => {
  uploadFiles(event.target.files);
  event.target.value = "";
});
$("folder-add").addEventListener("click", newFolder);
$("preview-close").addEventListener("click", () => $("preview-dialog").close());
$("share-cancel").addEventListener("click", () => $("share-dialog").close());
$("share-confirm").addEventListener("click", doShare);
document.addEventListener("dragover", (event) => {
  if (!event.dataTransfer?.types.includes("Files")) return;
  event.preventDefault();
  $("drop-zone").classList.add("dragging");
});
document.addEventListener("dragleave", (event) => {
  if (!event.relatedTarget) $("drop-zone").classList.remove("dragging");
});
document.addEventListener("drop", (event) => {
  if (!event.dataTransfer?.files.length) return;
  event.preventDefault();
  $("drop-zone").classList.remove("dragging");
  uploadFiles(event.dataTransfer.files);
});
load();
