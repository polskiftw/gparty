export const PAGE_HTML = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="robots" content="noindex,nofollow,noarchive">
<meta name="theme-color" content="#1e0754">
<title>GOONING.PARTY — The Private Debug Zone</title>
<link rel="stylesheet" href="/_/style.css">
<script src="/_/app.js" defer></script>
</head>
<body>
<a class="skip" href="#directory">Skip to files</a>
<div class="shell">
  <div class="topline"><span>★ WELCOME 2 THE WORLD WIDE WEB ★</span><span>PRIVATE MEMBERS ONLY</span></div>
  <header class="banner">
    <div class="sparkles" aria-hidden="true">★ ✶ ✦ ★</div>
    <h1>GOONING.PARTY</h1>
    <strong>~*~ THE PRIVATE DEBUG ZONE ~*~</strong>
    <p>UPLOAD YOUR PROBLEMS. SHARE YOUR LOGS. FIX THE THING.</p>
  </header>
  <div class="ticker"><span>★ VERIFIED CLIENT CERTIFICATE ★ SHARED FILE SYSTEM ★ NO ACCOUNTS, NO NONSENSE ★ FILES LIVE AT ACTUAL URLS ★</span></div>
  <nav class="nav" aria-label="Website navigation">
    <a href="/">📁 File browser</a>
    <a href="/gallery/">🛸 Original gallery</a>
    <span>🔒 AUTHORIZED</span>
  </nav>
  <main id="directory">
    <div class="section-header">
      <div><h2>★ FILE EXPLORER ★</h2><p>Everybody sees the same folders. All files stay private unless deliberately shared with AI.</p></div>
      <div class="actions">
        <button type="button" id="folder-add">📁 New folder</button>
        <button type="button" id="choose-files">⬆ Upload files</button>
      </div>
    </div>
    <nav class="crumbs" id="crumbs" aria-label="Current folder"></nav>
    <div id="drop-zone" class="drop" role="button" tabindex="0" aria-label="Drop files to upload or select files">
      <div class="drop-icon" aria-hidden="true">💾</div>
      <strong>DRAG &amp; DROP YOUR DISASTERS HERE</strong>
      <span>Or click this box to select files · Up to 64 MiB each</span>
    </div>
    <input id="file-picker" type="file" multiple hidden>
    <div class="table-heading"><strong>↓↓ CONTENTS OF THIS FOLDER ↓↓</strong><span id="count"></span></div>
    <div id="file-list" class="file-list" aria-live="polite"><p>Loading directory...</p></div>
    <div id="status" role="status" aria-live="polite"></div>
  </main>
  <footer>⚡ 100% CERTIFICATE-POWERED ⚡ PRIVATE FRIENDSHIP INTERNET ⚡ STILL UNDER CONSTRUCTION ⚡</footer>
</div>
<dialog id="preview-dialog" aria-labelledby="preview-title">
  <div class="dialog-heading"><h2 id="preview-title">FILE PREVIEW</h2><button type="button" id="preview-close">✕ Close</button></div>
  <p id="preview-info"></p>
  <div id="preview-body" class="preview-body"></div>
</dialog>
<dialog id="share-dialog" aria-labelledby="share-title">
  <h2 id="share-title">MAKE FILE AI-READABLE?</h2>
  <p id="share-filename"></p>
  <p>This copies the text to a separate <strong>private GitHub repository</strong> so ChatGPT can inspect it through the GitHub connector. Check logs for tokens, IP addresses, and personal data first.</p>
  <p><strong>IMPORTANT:</strong> GitHub keeps commit history. Deleting a copy later does not erase the earlier commits.</p>
  <div class="dialog-actions"><button type="button" id="share-cancel">No, keep private</button><button type="button" id="share-confirm">Yes, share with AI</button></div>
</dialog>
</body>
</html>`;
