// Cloudflare Worker -> ephemeral GitHub runner, preserving mTLS and keeping links in private R2.
// The runner alone has the login cookie; browser and Worker never receive it.
const PREFIX = "archive-jobs/";
const HOSTS = new Set(["www.reddit.com","reddit.com","old.reddit.com","new.reddit.com","np.reddit.com","redd.it"]);
const ID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
class ArchiveRequestError extends Error {
  constructor(message,status=400) { super(message); this.status=status; }
}
function json(data,status=200) {
  return new Response(JSON.stringify(data),{status,headers:{
    "content-type":"application/json; charset=utf-8","cache-control":"private, no-store","x-gparty-api":"1",
  }});
}
function validateUrl(input) {
  if(typeof input!=="string"||input.length>2048) throw new ArchiveRequestError("Paste one Reddit post URL.");
  let url;
  try { url=new URL(input.trim()); } catch { throw new ArchiveRequestError("Enter a complete https:// Reddit post URL."); }
  if(url.protocol!=="https:"||url.username||url.password||url.port||!HOSTS.has(url.hostname.toLowerCase()))
    throw new ArchiveRequestError("Only HTTPS Reddit post URLs are accepted.");
  const isShortLink = url.hostname.toLowerCase() === "redd.it";
  const pattern = isShortLink
    ? /^\/[a-z0-9]{4,16}\/?$/i
    : /^\/(?:r\/[^/]+\/(?:comments\/[a-z0-9]+|s\/[\w-]+)|comments\/[a-z0-9]+|gallery\/[a-z0-9]+)(?:\/|$)/i;
  if(!pattern.test(url.pathname))
    throw new ArchiveRequestError("Use a link to an individual Reddit post, not a feed or profile.");
  url.hash=""; url.search=""; return url.toString();
}
function validId(value) { if(!ID_PATTERN.test(value||"")) throw new ArchiveRequestError("Invalid archive job ID."); return value; }
function key(id,name) { return PREFIX+validId(id)+"/"+name; }
function safeFilename(input) {
  const name=String(input||"").replace(/[^A-Za-z0-9._-]/g,"-").slice(0,160);
  return name.endsWith(".tar.zst")?name:"reddit-post.tar.zst";
}
export async function submitArchiveJob(data,env) {
  try {
    const url=validateUrl(data?.url);
    if(!env.ARCHIVE_GITHUB_TOKEN) throw new ArchiveRequestError("Archive dispatch is not configured. Add the ARCHIVE_GITHUB_TOKEN Worker secret.",503);
    const id=crypto.randomUUID(),createdAt=new Date().toISOString();
    await env.PRIVATE_FILES_BUCKET.put(key(id,"request.json"),JSON.stringify({url,createdAt,version:1}),{
      httpMetadata:{contentType:"application/json",cacheControl:"private, no-store"},
    });
    let response;
    try {
      response=await fetch("https://api.github.com/repos/polskiftw/gparty/actions/workflows/archive-reddit-post.yml/dispatches",{
        method:"POST",headers:{
          "authorization":"Bearer "+env.ARCHIVE_GITHUB_TOKEN,
          "accept":"application/vnd.github+json","content-type":"application/json",
          "x-github-api-version":"2022-11-28","user-agent":"GParty-private-archive-dispatcher",
        },body:JSON.stringify({ref:"main",inputs:{job_id:id}}),
      });
    } catch { /* Report a sanitized dispatch failure. */ }
    if(!response||response.status!==204) {
      await env.PRIVATE_FILES_BUCKET.delete(key(id,"request.json"));
      throw new ArchiveRequestError("GitHub could not start the private archiver (HTTP "+(response?.status||"network error")+"). Verify ARCHIVE_GITHUB_TOKEN and workflow.",502);
    }
    return json({id,state:"queued",createdAt},202);
  } catch(err) { if(err instanceof ArchiveRequestError) return json({error:err.message},err.status); throw err; }
}
export async function archiveJobStatus(bucket,id) {
  try {
    validId(id);
    const req=await bucket.get(key(id,"request.json"));
    if(!req) return json({error:"Archive job not found."},404);
    const info=await req.json();
    if(Date.now()-Date.parse(info.createdAt)>86400000) return json({error:"Archive expired. Submit the post again."},410);
    const obj=await bucket.get(key(id,"status.json"));
    if(!obj) return json({id,state:"queued",message:"Awaiting an archive runner."});
    const status=await obj.json();
    if(!["queued","running","ready","failed"].includes(status.state)) return json({error:"Invalid archive status."},503);
    return json({id,state:status.state,message:String(status.message||"").slice(0,240),
      ...(status.state==="ready"?{filename:safeFilename(status.filename)}:{})});
  } catch(err) { if(err instanceof ArchiveRequestError) return json({error:err.message},err.status);throw err; }
}
export async function downloadArchive(bucket,id) {
  try {
    validId(id);
    const state=await archiveJobStatus(bucket,id);
    if(!state.ok) return state;
    const meta=await state.json();
    if(meta.state!=="ready") return json({error:"Archive is not finished yet."},409);
    const file=await bucket.get(key(id,"archive.tar.zst"));
    if(!file) return json({error:"Archive output is missing."},503);
    return new Response(file.body,{status:200,headers:{
      "content-type":"application/zstd",
      "content-disposition":"attachment; filename*=UTF-8''"+encodeURIComponent(meta.filename),
      "content-length":String(file.size),"cache-control":"private, no-store","x-content-type-options":"nosniff",
    }});
  } catch(err) { if(err instanceof ArchiveRequestError) return json({error:err.message},err.status);throw err; }
}
export {validateUrl};
