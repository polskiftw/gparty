import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { Script } from "node:vm";

const read=(name)=>readFileSync(new URL("../src/"+name,import.meta.url));
const page=read("page.js").toString(),script=read("chaos.js").toString(),css=read("style.css").toString();
const midi=read("internet-anthem.mid");
test("MIDI is genuine format-1 with multiple tracks and playable note on/off pairs",()=>{
  assert.equal(midi.toString("ascii",0,4),"MThd");
  assert.equal(midi.readUInt32BE(4),6);
  assert.equal(midi.readUInt16BE(8),1);
  assert.equal(midi.readUInt16BE(10),6);
  assert.equal(midi.readUInt16BE(12),480);
  assert.ok(midi.length>3000&&midi.length<100000);
  assert.ok(midi.includes(Buffer.from("MTrk")));
  assert.ok(midi.includes(Buffer.from("Neon Goblin Lead")));
  assert.ok(midi.includes(Buffer.from("Computer Percussion")));
  // Actually execute the exact in-browser MIDI parser against the real soundtrack.
  const begin=script.indexOf("  function parseMidi(buffer) {");
  const end=script.indexOf("\n  let noise = null;",begin);
  assert.ok(begin>=0&&end>begin,"embedded MIDI parser was found");
  const parse=new Script("("+script.slice(begin,end)+")").runInNewContext();
  const buffer=midi.buffer.slice(midi.byteOffset,midi.byteOffset+midi.byteLength);
  const song=parse(buffer);
  assert.ok(song.notes.length>500,"polyphonic MIDI has hundreds of real notes");
  assert.ok(song.length>26&&song.length<28);
  assert.ok(song.notes.some(n=>n.channel===9),"real MIDI percussion");
  assert.ok(song.notes.some(n=>n.channel===0),"real MIDI melody");
});
test("decorations are separate from the working file UI and contain no remote dependencies",()=>{
  for(const id of ["directory","folder-add","choose-files","drop-zone","file-picker","file-list","share-dialog"])assert.match(page,new RegExp('id="'+id+'"'));
  for(const id of ["visitor-number","online-number","music-button","fortune-button","worse-button","cursor-dust"])assert.match(page,new RegExp('id="'+id+'"'));
  assert.match(page,/script src="\/_\/chaos\.js" defer/);
  assert.match(page,/\/_\/internet-anthem\.mid/);
  assert.match(page,/\/_\/webmaster\.svg/);
  assert.doesNotMatch(page,/https?:\/\/(?!(?:gooning\.party))[^"' ]+\.(js|css|svg|mp3)/);
  assert.match(css,/@media\(prefers-reduced-motion:reduce\)/);
  assert.match(css,/@media\(pointer:fine\)/);
  assert.match(script,/\/_\/api\/presence/);
  assert.match(script,/\/_\/internet-anthem\.mid/);
  assert.doesNotMatch(script,/eval\(|document\.write|innerHTML/);
  assert.doesNotThrow(()=>new Script(script));
});
test("Cloudflare bindings and protected assets exist in both local and manual-deploy config",()=>{
  const web=readFileSync(new URL("../../web/src/worker.js",import.meta.url),"utf8");
  const wrangler=JSON.parse(readFileSync(new URL("../../web/wrangler.jsonc",import.meta.url),"utf8"));
  const workflow=readFileSync(new URL("../../../.github/workflows/deploy-private-file-site.yml",import.meta.url),"utf8");
  assert.match(web,/authenticated\(request\)/);
  assert.match(web,/export \{ VisitorCounter \}/);
  assert.ok(wrangler.durable_objects.bindings.some(b=>b.name==="VISITOR_COUNTER"&&b.class_name==="VisitorCounter"));
  assert.ok(wrangler.migrations.some(m=>m.new_sqlite_classes?.includes("VisitorCounter")));
  assert.match(workflow,/new_sqlite_classes = \["VisitorCounter"\]/);
  assert.match(workflow,/globs = \["\*\*\/\*\.mid"\]/);
  assert.ok(wrangler.rules.some(r=>r.type==="Data"&&r.globs.includes("**/*.mid")));
});
