export const PAGE_HTML = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="robots" content="noindex,nofollow,noarchive">
<meta name="theme-color" content="#1e0754">
<title>~*~ GOONING.PARTY ~*~ THE EXTREMELY PRIVATE INTERNET EXPERIENCE</title>
<link rel="stylesheet" href="/_/style.css">
<script src="/_/app.js" defer></script>
<script src="/_/chaos.js" defer></script>
</head>
<body>
<a class="skip" href="#directory">Skip the digital pollution and go to files</a>
<canvas id="cursor-dust" aria-hidden="true"></canvas>
<div class="shell">
  <div class="topline"><span>★ WELCOME 2 THE WORLD WIDE WEB ★</span><span>PRIVATE MEMBERS ONLY · <time id="web-clock" aria-label="Your local time">00:00:00</time></span></div>
  <header class="banner">
    <div class="sparkles" aria-hidden="true">✦ ★ ✶ ✹ ✦ ★ ✶ ✹ ★ ✦</div>
    <div class="banner-alien" aria-hidden="true">👽<span>WEBMASTER ONLINE</span></div>
    <h1>GOONING.PARTY</h1>
    <strong>~*~ THE PRIVATE DEBUG ZONE ~*~</strong>
    <p>UPLOAD YOUR PROBLEMS. SHARE YOUR LOGS. FIX THE THING. REGRET YOUR AESTHETIC CHOICES.</p>
    <div class="hero-warning">⚠ WARNING: THIS PAGE WAS APPROVED BY ZERO DESIGNERS ⚠</div>
  </header>
  <div class="ticker" role="marquee" aria-label="Verified certificate; big uploads; MIDI goblins; site under construction"><span class="ticker-scroll" aria-hidden="true">★ VERIFIED CLIENT CERTIFICATE ★ EXTREMELY LARGE FILE UPLOADS ★ PRIVATE GITHUB DEBUG BRIDGE ★ 100% HANDCRAFTED INTERNET POLLUTION ★ FREE MIDI GOBLINS ★ UNDER CONSTRUCTION SINCE FOREVER ★ THIS TEXT IS MOVING FOR ABSOLUTELY NO REASON ★</span></div>
  <nav class="nav" aria-label="Website navigation">
    <a href="/">📁 File browser</a>
    <span>🔒 AUTHORIZED</span>
  </nav>
  <section class="chaos-strip" aria-label="Very important website statistics and soundtrack">
    <div class="stat-booth"><strong>VISITOR NUMBER</strong><output id="visitor-number" aria-label="Total site visits">--------</output><small>REAL HITS · NOT A RANDOM NUMBER</small></div>
    <div class="stat-booth"><strong>GOBLINS ONLINE NOW</strong><output id="online-number" aria-label="Recent active visitors">---</output><small>LIVE · LAST 60 SECONDS</small></div>
    <div class="music-booth"><strong>♫ 8-BIT INTERNET ANTHEM ♫</strong><button id="music-button" type="button" aria-pressed="false">▶ ENABLE 8-BIT MIDI</button><a href="/_/internet-anthem.mid" download="internet-anthem.mid">↓ DOWNLOAD ACTUAL MIDI FILE ↓</a><small id="music-status" role="status">BROWSER AUDIO AWAITS INTERACTION</small></div>
  </section>
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
      <span>Or click to choose files · Multipart uploads for large files</span>
    </div>
    <input id="file-picker" type="file" multiple hidden>
    <div class="table-heading"><strong>↓↓ CONTENTS OF THIS FOLDER ↓↓</strong><span id="count"></span></div>
    <div id="file-list" class="file-list" aria-live="polite"><p>Loading directory...</p></div>
    <div id="status" role="status" aria-live="polite"></div>
  </main>
  <section class="cyber-arcade" aria-label="Completely pointless extras">
    <div class="webmaster-card"><strong>☻ MEET THE WEBMASTER ☻</strong><img src="/_/webmaster.svg" width="240" height="170" alt="Original pixel art: green alien in a UFO abducting a floppy disk"><p>EXPERIENCE: YES. QUALIFICATIONS: ASK AGAIN LATER.</p></div>
    <div class="oracle-card"><strong>🔮 THE CYBER ORACLE 3000 🔮</strong><p id="fortune-text" aria-live="polite">THE INTERNET PREDICTS GREAT CONFUSION.</p><button id="fortune-button" type="button">REQUEST A TERRIBLE PROPHECY</button><button id="worse-button" type="button" aria-pressed="false">MAKE THIS SITE WORSE</button></div>
  </section>
  <div class="badge-wall" aria-label="Extremely prestigious imaginary web awards">
    <span class="web-badge"><b>HTML</b> POWERED</span><span class="web-badge"><b>56K</b> READY</span><span class="web-badge"><b>FREE</b> PIXELS</span>
    <span class="web-badge"><b>NO</b> ADULTS</span><span class="web-badge"><b>R2</b> INSIDE</span><span class="web-badge"><b>100%</b> CURSED</span>
    <span class="web-badge"><b>NO FLASH</b> NEEDED</span><span class="web-badge"><b>MIDI</b> FOREVER</span><span class="web-badge"><b>UNDER</b> CONSTRUCTION</span>
  </div>
  <footer><div class="construction-text">🚧 THIS WEBSITE WILL NEVER BE FINISHED 🚧</div>⚡ 100% CERTIFICATE-POWERED ⚡ PRIVATE FRIENDSHIP INTERNET ⚡ STILL UNDER CONSTRUCTION ⚡<p>© 1997–FOREVER · DESIGN BY A RACCOON WITH FRONTEND ACCESS · YOUR ACTUAL FILES STILL WORK.</p></footer>
</div>
<dialog id="preview-dialog" aria-labelledby="preview-title">
  <div class="dialog-heading"><h2 id="preview-title">FILE PREVIEW</h2><button type="button" id="preview-close">✕ Close</button></div>
  <p id="preview-info"></p>
  <div id="preview-body" class="preview-body"></div>
</dialog>
<dialog id="share-dialog" aria-labelledby="share-title">
  <h2 id="share-title">MAKE FILE AI-READABLE?</h2>
  <p id="share-filename"></p>
  <p>This exports text to a separate <strong>private GitHub repository</strong>. Large logs are divided into small indexed chunks that ChatGPT can read through the GitHub connector. Check for tokens, IP addresses, and personal data first.</p>
  <p><strong>IMPORTANT:</strong> GitHub keeps commit history. Deleting a copy later does not erase the earlier commits.</p>
  <div class="dialog-actions"><button type="button" id="share-cancel">No, keep private</button><button type="button" id="share-confirm">Yes, share with AI</button></div>
</dialog>
</body>
</html>`;
