"use strict";
// The useless parts are isolated from the real file manager. No dependencies, no tracking.
(() => {
  const byId = (id) => document.getElementById(id);
  const reduced = window.matchMedia("(prefers-reduced-motion: reduce)");
  const fortunes = [
    "YOUR FLOPPY DISK HAS BEEN PROMOTED TO MANAGEMENT.",
    "ERROR 418: THIS WEBSITE IS A TEAPOT.",
    "SOMEWHERE, A WEB DESIGNER JUST FELT A DISTURBANCE.",
    "YOUR NEXT UPLOAD WILL BE BLESSED BY THREE PIXEL GOBLINS.",
    "CONGRATULATIONS: YOU HAVE WON 0 FREE MEGABYTES.",
    "THE INTERNET HAS REQUESTED A REFUND.",
    "SYSTEM REQUIREMENTS: 1 MONITOR AND AN UNREASONABLE AMOUNT OF CONFIDENCE.",
    "THIS IS WHAT HAPPENS WHEN NOBODY SAYS NO.",
    "YOU ARE BEING WATCHED BY A DRAWING OF A FLOPPY DISK.",
    "WARNING: YOUR WEBMASTER HAS DISCOVERED CSS.",
  ];
  function fortune() {
    const display = byId("fortune-text");
    if (display) display.textContent = fortunes[Math.floor(Math.random() * fortunes.length)];
  }
  byId("fortune-button")?.addEventListener("click", () => { fortune(); burst(innerWidth / 2, Math.min(innerHeight - 20, 180), 36); });
  byId("worse-button")?.addEventListener("click", (event) => {
    const on = document.documentElement.classList.toggle("too-much");
    event.currentTarget.setAttribute("aria-pressed", String(on));
    event.currentTarget.textContent = on ? "UNDO THE BAD DECISION" : "MAKE THIS SITE WORSE";
    burst(innerWidth / 2, Math.min(innerHeight - 20, 150), 45);
  });
  const clock = byId("web-clock");
  function clockTick() { if (clock) clock.textContent = new Date().toLocaleTimeString([], {hour:"2-digit",minute:"2-digit",second:"2-digit"}); }
  clockTick();
  setInterval(clockTick, 1000);
  // Anonymous per-tab presence; a heartbeat every 20 s gives a real "online" reading.
  const visitors = byId("visitor-number"), online = byId("online-number");
  let session;
  try {
    session = sessionStorage.getItem("gparty-presence-id");
    if (!session) { session = crypto.randomUUID(); sessionStorage.setItem("gparty-presence-id", session); }
  } catch { session = crypto.randomUUID(); }
  async function heartbeat(action) {
    if (document.hidden) return;
    try {
      const response = await fetch("/_/api/presence", {
        method:"POST", cache:"no-store", credentials:"same-origin",
        headers:{"content-type":"application/json","x-gparty-write":"1"},
        body:JSON.stringify({sessionId:session,action}),
      });
      if (!response.ok) throw new Error("counter unavailable");
      const result = await response.json();
      if (visitors) visitors.textContent = String(result.visits).padStart(8,"0");
      if (online) online.textContent = String(result.online).padStart(3,"0");
    } catch {
      if (visitors && visitors.textContent === "--------") visitors.textContent = "OFFLINE";
      if (online && online.textContent === "---") online.textContent = "???";
    }
  }
  heartbeat("hello");
  setInterval(() => heartbeat("beat"), 20000);
  document.addEventListener("visibilitychange", () => { if (!document.hidden) heartbeat("beat"); });

  // Distinct shapes and outlines make the trail legible without color discrimination.
  const canvas = byId("cursor-dust");
  const pen = canvas?.getContext("2d");
  let width = 0, height = 0, dpr = 1, pending = false, last = 0;
  const particles = [];
  function resize() {
    if (!pen) return;
    dpr = Math.min(window.devicePixelRatio || 1, 2);
    width = innerWidth; height = innerHeight;
    canvas.width = Math.round(width * dpr); canvas.height = Math.round(height * dpr);
    pen.setTransform(dpr, 0, 0, dpr, 0, 0);
  }
  function animate() {
    if (!pen) return;
    pending = false;
    pen.clearRect(0, 0, width, height);
    const now = performance.now();
    for (let i = particles.length - 1; i >= 0; i--) {
      const p = particles[i], f = (now - p.birth) / p.life;
      if (f >= 1) { particles.splice(i, 1); continue; }
      const x = p.x + p.dx * f, y = p.y + p.dy * f + 16 * f * f;
      const size = (1 - f) * p.size;
      pen.save();
      pen.globalAlpha = 1 - f;
      pen.translate(x, y);
      pen.rotate(p.spin * f);
      pen.strokeStyle = "#100024"; pen.lineWidth = 2.2;
      pen.fillStyle = ["#ffffff","#ffe436","#6bfff6","#ff74ea"][p.color];
      pen.beginPath();
      if (p.shape === 0) {
        pen.moveTo(-size, 0); pen.lineTo(size, 0);
        pen.moveTo(0, -size); pen.lineTo(0, size);
        pen.moveTo(-size * .5, -size * .5); pen.lineTo(size * .5, size * .5);
      } else if (p.shape === 1) {
        pen.rect(-size * .55, -size * .55, size * 1.1, size * 1.1);
      } else {
        pen.moveTo(0, -size); pen.lineTo(size, size); pen.lineTo(-size, size); pen.closePath();
      }
      pen.stroke(); pen.fill(); pen.restore();
    }
    if (particles.length) { pending = true; requestAnimationFrame(animate); }
  }
  function burst(x, y, count = 3) {
    if (!pen || reduced.matches) return;
    for (let i = 0; i < count; i++) {
      const angle = Math.random() * Math.PI * 2, distance = 12 + Math.random() * (count > 6 ? 115 : 24);
      particles.push({x,y,dx:Math.cos(angle)*distance,dy:Math.sin(angle)*distance - 10,
        birth:performance.now(),life:450+Math.random()*520,size:3+Math.random()*5,
        spin:Math.random()*5, color:i%4, shape:i%3});
    }
    if (particles.length > 150) particles.splice(0,particles.length - 150);
    if (!pending) { pending = true; requestAnimationFrame(animate); }
  }
  if (pen) {
    resize();
    window.addEventListener("resize", resize, {passive:true});
    window.addEventListener("pointermove", (event) => {
      if (event.pointerType === "touch" || performance.now() - last < 22) return;
      last = performance.now(); burst(event.clientX,event.clientY,3);
    }, {passive:true});
    window.addEventListener("pointerdown", (event) => { if (event.pointerType !== "touch") burst(event.clientX,event.clientY,18); }, {passive:true});
    reduced.addEventListener("change", () => { if (reduced.matches) {particles.length=0; pen.clearRect(0,0,width,height);} });
  }

  // Play the REAL .mid file through a tiny homebuilt Web Audio chiptune synthesizer.
  // This is MIDI playback, not a disguised MP3 / third-party streaming dependency.
  const musicButton = byId("music-button"), musicStatus = byId("music-status");
  let context = null, master = null, timer = null, song = null;
  let playing = false, nextNote = 0, loopStart = 0;
  const AudioEngine = window.AudioContext || window.webkitAudioContext;
  function setMusicState(message) {
    if (musicStatus) musicStatus.textContent = message;
    if (musicButton) {
      musicButton.textContent = playing ? "■ SILENCE THIS ABOMINATION" : "▶ ENABLE 8-BIT MIDI";
      musicButton.setAttribute("aria-pressed", String(playing));
    }
  }
  // Read the generated standard MIDI format: track headers, VLQs, notes, tempo.
  function parseMidi(buffer) {
    const view = new DataView(buffer);
    let at = 0;
    const u8 = () => view.getUint8(at++);
    const u16 = () => { const v=view.getUint16(at); at+=2; return v; };
    const u32 = () => { const v=view.getUint32(at); at+=4; return v; };
    const chars = (n) => { let s=""; for(let i=0;i<n;i++) s+=String.fromCharCode(u8()); return s; };
    const vlq = () => { let n=0,b; do {b=u8();n=(n<<7)|(b&127);} while(b&128); return n; };
    if (chars(4)!=="MThd" || u32()!==6) throw new Error("Not a standard MIDI file");
    u16(); const tracks=u16(), division=u16();
    if (!division || division & 32768) throw new Error("Unsupported MIDI clock");
    let tempo=500000, endTick=0;
    const notes = [];
    for(let track=0;track<tracks;track++) {
      if (chars(4)!=="MTrk") throw new Error("Broken MIDI track");
      const bytes=u32(), end=at+bytes;
      let tick=0, running=0;
      const active = new Map();
      while(at<end) {
        tick+=vlq();
        let status=u8();
        if(status<128) {at--;status=running;} else if(status<240) running=status;
        if(status===255) {
          const type=u8(), length=vlq();
          if(type===81 && length===3) tempo=(u8()<<16)|(u8()<<8)|u8();
          else at+=length;
        } else if(status===240||status===247) at+=vlq();
        else {
          const op=status&240,channel=status&15;
          const a=u8(),b=(op===192||op===208)?0:u8();
          const key=channel+":"+a;
          if(op===144 && b>0) active.set(key,{tick,velocity:b,channel,note:a,track});
          if(op===128 || (op===144 && b===0)) {
            const started=active.get(key);
            if(started) {notes.push({...started,end:tick});active.delete(key);}
          }
        }
      }
      if(at!==end) throw new Error("MIDI track has invalid length");
      endTick=Math.max(endTick,tick);
    }
    const secondsPerTick=tempo/1000000/division;
    return {notes:notes.map(n=>({...n, start:n.tick*secondsPerTick,
      duration:Math.max(.035,(n.end-n.tick)*secondsPerTick)})).sort((a,b)=>a.start-b.start),
      length:endTick*secondsPerTick};
  }
  let noise = null;
  function initSynth() {
    if (!context) {
      context = new AudioEngine();
      master = context.createGain(); master.gain.value = .11;
      master.connect(context.destination);
      noise=context.createBuffer(1, Math.floor(context.sampleRate*.25),context.sampleRate);
      const values=noise.getChannelData(0);
      let seed=123456789;
      for(let i=0;i<values.length;i++) {seed=(1664525*seed+1013904223)>>>0;values[i]=(seed/4294967296)*2-1;}
    }
  }
  function playTone(n, when) {
    if (!context || !master) return;
    const percussion = n.channel===9;
    if (percussion && n.note!==36 && n.note!==38 && n.note!==42) return;
    const duration=percussion?(n.note===36 ? 0.14 : 0.065):Math.min(n.duration,0.80);
    const gain=context.createGain();
    gain.gain.setValueAtTime(.0001,when);
    const level=percussion?(n.note===36 ? 0.65 : 0.18):(n.track===1 ? 0.65 : n.track===2 ? 0.22 : n.track===3 ? 0.46 : 0.14);
    gain.gain.exponentialRampToValueAtTime(level, when+.007);
    gain.gain.exponentialRampToValueAtTime(.0001, when+duration);
    gain.connect(master);
    if(percussion && n.note!==36) {
      const source=context.createBufferSource();source.buffer=noise;
      source.connect(gain);source.start(when);source.stop(when+duration+.003);
    } else {
      const osc=context.createOscillator();
      osc.type=percussion?"sine":(n.track===1?"square":n.track===2?"square":n.track===3?"triangle":"sawtooth");
      const hz=percussion?115:440*Math.pow(2,(n.note-69)/12);
      osc.frequency.setValueAtTime(hz,when);
      if(percussion) osc.frequency.exponentialRampToValueAtTime(42,when+.13);
      osc.connect(gain);osc.start(when);osc.stop(when+duration+.01);
    }
  }
  function schedule() {
    if (!playing || !song || !context || context.state!=="running") return;
    const horizon=context.currentTime+.22;
    while(loopStart+song.notes[nextNote].start<horizon) {
      const note=song.notes[nextNote];
      playTone(note,Math.max(context.currentTime+.002,loopStart+note.start));
      if(++nextNote>=song.notes.length){nextNote=0;loopStart+=song.length;}
    }
  }
  async function start() {
    if (!AudioEngine) {setMusicState("NO AUDIO HARDWARE. HOW VERY 1996.");return;}
    try {
      initSynth();
      const resumed=context.resume(); // Must occur inside a real input gesture on modern browsers.
      if (!song) {
        const response=await fetch("/_/internet-anthem.mid",{cache:"no-store"});
        if(!response.ok) throw new Error("MIDI not found");
        song=parseMidi(await response.arrayBuffer());
        if (!song.notes.length || song.length<=0) throw new Error("MIDI is empty");
      }
      await resumed;
      if(context.state!=="running") {setMusicState("CLICK PLAY TO UNLOCK BROWSER AUDIO");return;}
      if(playing) return;
      playing=true;nextNote=0;loopStart=context.currentTime+.075;
      timer=setInterval(schedule,70);schedule();setMusicState("NOW PLAYING: INTERNET ANTHEM.MID ♫");
    } catch(error) {
      setMusicState("MIDI GREMLIN: "+error.message);
      console.warn("GParty 8-bit audio unavailable",error);
    }
  }
  function stop() {
    playing=false;
    if(timer) clearInterval(timer);
    timer=null;
    if(context) {context.close();context=null;master=null;noise=null;}
    setMusicState("SOUND DISABLED. COWARDICE ACCEPTED.");
  }
  musicButton?.addEventListener("click", () => playing?stop():start());
  setMusicState("WAITING FOR HUMAN INTERACTION (BROWSER AUDIO RULES)");
  // Browsers forbid unsolicited audio. Begin after the first *non-music* gesture,
  // leaving the file-management event itself completely untouched.
  let tried=false;
  const firstGesture=(event) => {
    if (tried || event.target?.closest?.("#music-button")) return;
    tried=true;start();
  };
  document.addEventListener("pointerdown",firstGesture,{passive:true});
  document.addEventListener("keydown",firstGesture,{passive:true});
  document.addEventListener("visibilitychange", () => {
    if (!context || !playing) return;
    if(document.hidden) context.suspend();
    else context.resume().then(schedule).catch(()=>{});
  });
})();
