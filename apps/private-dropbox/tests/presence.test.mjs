import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const source=readFileSync(new URL("../src/presence.js",import.meta.url),"utf8");
const {VisitorCounter}=await import("data:text/javascript;base64,"+Buffer.from(source).toString("base64"));
class MemoryStore {
  constructor(){this.map=new Map();}
  async get(key){return this.map.get(key);}
  async put(key,value){this.map.set(key,value);}
  async delete(key){this.map.delete(key);}
  async list({prefix}){return new Map([...this.map].filter(([key])=>key.startsWith(prefix)));}
}
function report(counter,sessionId,action="hello"){
  return counter.fetch(new Request("https://presence.internal/update",{
    method:"POST",headers:{"content-type":"application/json"},
    body:JSON.stringify({sessionId,action}),
  }));
}
const A="123e4567-e89b-42d3-a456-426614174000",B="123e4567-e89b-42d3-a456-426614174001";

test("a real counter counts distinct active sessions; heartbeats and folder navigations do not invent more hits",async()=>{
  const storage=new MemoryStore(),counter=new VisitorCounter({storage});
  assert.deepEqual(await (await report(counter,A)).json(),{visits:1,online:1,ttlSeconds:60});
  assert.deepEqual(await (await report(counter,A,"beat")).json(),{visits:1,online:1,ttlSeconds:60});
  assert.deepEqual(await (await report(counter,B)).json(),{visits:2,online:2,ttlSeconds:60});
  assert.equal(storage.map.get("total-visits"),2);
  assert.equal(storage.map.size,3);
});
test("expired tab sessions are discarded and the lifetime hit count persists",async()=>{
  const storage=new MemoryStore(),counter=new VisitorCounter({storage});
  await report(counter,A);
  storage.map.set("online:"+A,0);
  assert.deepEqual(await (await report(counter,B)).json(),{visits:2,online:1,ttlSeconds:60});
  assert.equal(storage.map.has("online:"+A),false);
  assert.deepEqual(await (await report(counter,A,"beat")).json(),{visits:3,online:2,ttlSeconds:60});
});
test("counter accepts only validated UUID updates and does not leak tracking data",async()=>{
  const storage=new MemoryStore(),counter=new VisitorCounter({storage});
  assert.equal((await report(counter,"bad-id")).status,400);
  assert.equal((await report(counter,A,"delete")).status,400);
  assert.equal((await counter.fetch(new Request("https://presence.internal/update"))).status,405);
  const ok=await report(counter,A);
  assert.equal(ok.headers.get("x-gparty-api"),"1");
  assert.equal(ok.headers.get("cache-control"),"private, no-store");
  assert.deepEqual([...storage.map.keys()].sort(),["online:"+A,"total-visits"].sort());
});
