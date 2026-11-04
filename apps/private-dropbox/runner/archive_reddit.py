"""Private Reddit HTML post archiver for ephemeral GitHub runners.

No Reddit OAuth and no Reddit REST/JSON API requests: reads logged-in HTML.
Archives are stored in private R2, then served only by the mTLS GParty Worker.
"""
from __future__ import annotations

import argparse
import base64
import datetime as dt
import html
import http.cookiejar
import json
import os
import pathlib
import re
import subprocess
import urllib.parse
from typing import Any

import boto3
import requests
from bs4 import BeautifulSoup

JOB_RE = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
HOSTS = {"reddit.com", "www.reddit.com", "old.reddit.com", "new.reddit.com", "np.reddit.com", "redd.it"}
MEDIA_HOSTS = {"i.redd.it", "preview.redd.it", "external-preview.redd.it", "v.redd.it", "packaged-media.redd.it", "i.imgur.com"}
UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:153.0) Gecko/20100101 Firefox/153.0"
MAX_HTML = 8 * 1024 * 1024
MAX_MEDIA = 512 * 1024 * 1024
MAX_TOTAL = 640 * 1024 * 1024
MEDIA_PATTERN = re.compile(r'https?://(?:i\.redd\.it|preview\.redd\.it|v\.redd\.it|packaged-media\.redd\.it)/[A-Za-z0-9_/?&=%+.,;:@~!-]+', re.I)


class ArchiveProblem(Exception):
    pass


def check_url(value: str, *, media: bool = False) -> str:
    url = urllib.parse.urlsplit(value)
    if (url.scheme != "https" or url.username or url.password or url.port or
        (url.hostname or "").lower() not in (MEDIA_HOSTS if media else HOSTS)):
        raise ArchiveProblem("Unexpected destination; restricted to Reddit and its media CDNs")
    return value


def sanitize(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "-", value).strip("-._")[:70] or "post"


def decode_fragment(value: str) -> str:
    return html.unescape(value).replace(r"\/", "/").replace(r"\u0026", "&").replace(r"\u003d", "=")


def parse_post_html(text: str, url: str) -> dict[str, Any]:
    soup = BeautifulSoup(text, "html.parser")
    post = soup.select_one("shreddit-post") or soup.select_one("div.thing[data-fullname^='t3_']")
    post_html = str(post) if post else ""
    def metadata(key: str) -> str:
        tag = soup.select_one(f'meta[property="{key}"]') or soup.select_one(f'meta[name="{key}"]')
        return str(tag.get("content", "")) if tag else ""
    title = (post.get("post-title") if post else "") or metadata("og:title") or (soup.title.get_text() if soup.title else "Reddit post")
    author = (post.get("author") if post else "") or "unknown"
    subreddit = (post.get("subreddit-name") if post else "") or ""
    body = ""
    if post:
        body_node = post.select_one('[slot="text-body"]') or post.select_one("[data-test-id='post-content']")
        if body_node:
            body = body_node.get_text("\n", strip=True)
    if not body:
        body = metadata("og:description")
    # Only scan the selected post, never suggested content, ads, or avatars.
    images: list[str] = []
    videos: list[str] = []
    seen: set[str] = set()
    candidates: list[str] = []
    if post:
        for el in post.select("img[src], a[href], video[src], source[src], shreddit-player"):
            for attr in ("src", "href", "poster", "dash-url", "source", "content-href"):
                raw = el.get(attr)
                if raw:
                    candidates.append(decode_fragment(str(raw)))
        candidates.extend(decode_fragment(x) for x in MEDIA_PATTERN.findall(decode_fragment(post_html)))
    if not candidates:
        # OpenGraph is a fallback when the actual post markup is rendered via JS.
        candidates.extend([metadata("og:image"), metadata("og:video"), metadata("twitter:player:stream")])
    for candidate in candidates:
        try:
            checked = check_url(candidate, media=True)
        except (ArchiveProblem, ValueError):
            continue
        parsed = urllib.parse.urlsplit(checked)
        if parsed.hostname == "v.redd.it":
            match = re.match(r"/([A-Za-z0-9_-]{5,40})", parsed.path)
            if match and match.group(1) not in videos:
                videos.append(match.group(1))
            continue
        if parsed.hostname not in {"i.redd.it", "preview.redd.it", "external-preview.redd.it", "i.imgur.com"}:
            continue
        path = urllib.parse.unquote(parsed.path)
        if not re.search(r"\.(?:jpe?g|png|webp|gif)$", path, re.I):
            continue
        identity = path.lower()
        if identity in seen:
            continue
        seen.add(identity)
        images.append(checked)
    return {
        "title": html.unescape(str(title)), "author": str(author), "subreddit": str(subreddit),
        "body": body, "url": url, "images": images, "videos": videos,
        "htmlPostFound": post is not None,
        "declaredPostType": post.get("post-type", "") if post else "",
    }


def get_page(session: requests.Session, url: str) -> tuple[str, str]:
    current = check_url(url)
    for _ in range(5):
        response = session.get(current, allow_redirects=False, timeout=60, stream=True,
            headers={"Accept": "text/html,application/xhtml+xml"})
        if response.is_redirect:
            location = response.headers.get("Location", "")
            response.close()
            current = check_url(urllib.parse.urljoin(current, location))
            continue
        if response.status_code != 200:
            raise ArchiveProblem(f"Reddit page returned HTTP {response.status_code} with login cookies")
        if "html" not in response.headers.get("Content-Type", "").lower():
            raise ArchiveProblem("Reddit returned something other than an HTML page")
        chunks: list[bytes] = []
        count = 0
        try:
            for chunk in response.iter_content(65536):
                count += len(chunk)
                if count > MAX_HTML:
                    raise ArchiveProblem("Post HTML exceeds the 8 MiB limit")
                chunks.append(chunk)
        finally:
            response.close()
        return b"".join(chunks).decode("utf-8", errors="replace"), current
    raise ArchiveProblem("Too many Reddit page redirects")


def download_image(session: requests.Session, url: str, dest: pathlib.Path, max_bytes: int) -> int:
    current = check_url(url, media=True)
    for _ in range(5):
        response = session.get(current, allow_redirects=False, timeout=90, stream=True)
        if response.is_redirect:
            target = response.headers.get("Location", "")
            response.close()
            current = check_url(urllib.parse.urljoin(current, target), media=True)
            continue
        if response.status_code != 200:
            response.close()
            raise ArchiveProblem(f"Image CDN returned HTTP {response.status_code}")
        if "text/html" in response.headers.get("Content-Type", ""):
            response.close()
            raise ArchiveProblem("Image CDN returned an HTML page")
        count = 0
        try:
            with dest.open("wb") as handle:
                for chunk in response.iter_content(1024 * 1024):
                    count += len(chunk)
                    if count > min(MAX_MEDIA, max_bytes):
                        raise ArchiveProblem("Media or total archive exceeds the size limit")
                    handle.write(chunk)
        finally:
            response.close()
        if not count:
            raise ArchiveProblem("Image CDN returned an empty file")
        return count
    raise ArchiveProblem("Too many image redirects")


def download_video(video_id: str, dest: pathlib.Path, cookie_file: pathlib.Path, ua: str) -> int:
    # Feed yt-dlp the direct DASH manifest, not the Reddit post. No Reddit
    # extractor or Reddit API endpoint is invoked.
    manifest = f"https://v.redd.it/{video_id}/DASHPlaylist.mpd"
    try:
        completed = subprocess.run([
            "yt-dlp", "--no-playlist", "--no-progress", "--no-warnings",
            "--cookies", str(cookie_file), "--user-agent", ua, "--retries", "3",
            "--format", "bestvideo+bestaudio/best", "--merge-output-format", "mp4",
            "--output", str(dest), manifest,
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=480, check=False)
    except subprocess.TimeoutExpired as ex:
        raise ArchiveProblem("Video download timed out") from ex
    if completed.returncode != 0:
        raise ArchiveProblem("Direct Reddit video manifest could not be downloaded")
    actual = dest if dest.exists() else dest.with_suffix(".mkv")
    if not actual.exists():
        raise ArchiveProblem("Video downloader created no media")
    if actual != dest:
        actual.rename(dest)
    if dest.stat().st_size > MAX_MEDIA:
        raise ArchiveProblem("Video exceeds the 512 MiB limit")
    return dest.stat().st_size


def cookies_from_secret(path: pathlib.Path) -> None:
    encoded = os.getenv("REDDIT_COOKIES_BASE64", "").strip()
    if not encoded:
        raise ArchiveProblem("Existing REDDIT_COOKIES_BASE64 secret is missing")
    try:
        text = base64.b64decode(encoded, validate=True).decode("utf-8-sig")
    except Exception as ex:
        raise ArchiveProblem("REDDIT_COOKIES_BASE64 is not valid Netscape cookie text") from ex
    if "reddit.com" not in text.lower() or not text.lstrip().startswith(("# Netscape HTTP Cookie File", "# HTTP Cookie File")):
        raise ArchiveProblem("REDDIT_COOKIES_BASE64 must contain a Netscape cookie export for reddit.com")
    path.write_text(text, encoding="utf-8")
    path.chmod(0o600)


def storage():
    required = ["R2_ACCOUNT_ID", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY", "PRIVATE_FILES_R2_BUCKET"]
    missing = [name for name in required if not os.getenv(name)]
    if missing:
        raise ArchiveProblem("Archive R2 runner credentials are missing: " + ", ".join(missing))
    return boto3.client("s3", endpoint_url=f"https://{os.environ['R2_ACCOUNT_ID']}.r2.cloudflarestorage.com",
        aws_access_key_id=os.environ["R2_ACCESS_KEY_ID"],
        aws_secret_access_key=os.environ["R2_SECRET_ACCESS_KEY"], region_name="auto"), os.environ["PRIVATE_FILES_R2_BUCKET"]


def r2key(job: str, name: str) -> str:
    if not JOB_RE.fullmatch(job):
        raise ArchiveProblem("Invalid job ID")
    return f"archive-jobs/{job}/{name}"


def set_status(client, bucket: str, job: str, state: str, message: str, filename: str = "") -> None:
    body = json.dumps({"state": state, "message": message, "filename": filename,
        "updatedAt": dt.datetime.now(dt.timezone.utc).isoformat()}).encode()
    client.put_object(Bucket=bucket, Key=r2key(job, "status.json"), Body=body,
        ContentType="application/json", CacheControl="private, no-store")


def archive_post(job: str, output_dir: pathlib.Path) -> dict[str, str]:
    if os.getenv("GPARTY_SOURCE_ROUTE_VERIFIED") != "1":
        raise ArchiveProblem("Tailscale home exit route was not verified")
    client, bucket = storage()
    payload = json.loads(client.get_object(Bucket=bucket, Key=r2key(job,"request.json"))["Body"].read())
    link = check_url(payload["url"])
    cookie_file = output_dir / "cookies.txt"
    cookies_from_secret(cookie_file)
    try:
        session = requests.Session()
        session.headers.update({"User-Agent":os.getenv("REDDIT_BROWSER_UA") or UA,"Accept-Language":"en-US,en;q=0.8"})
        jar = http.cookiejar.MozillaCookieJar(str(cookie_file))
        jar.load(ignore_discard=True, ignore_expires=False)
        if not any("reddit.com" in c.domain for c in jar):
            raise ArchiveProblem("Cookie file has no usable Reddit cookies; re-export Firefox login")
        session.cookies.update(jar)
        page, resolved = get_page(session, link)
        post = parse_post_html(page, resolved)
        if not post["htmlPostFound"]:
            raise ArchiveProblem("Reddit did not return post HTML; refusing to archive a login, challenge or error page")
        id_match = re.search(r"/(?:comments|gallery)/([a-z0-9]{4,16})(?:/|$)",urllib.parse.urlsplit(resolved).path,re.I)
        if not id_match:
            raise ArchiveProblem("Reddit did not resolve the link to a post permalink")
        post_id = id_match.group(1).lower()
        folder = output_dir / ("reddit-"+post_id)
        media = folder / "media"
        media.mkdir(parents=True)
        (folder/"post.json").write_text(json.dumps({k:v for k,v in post.items() if k not in ("images","videos")},ensure_ascii=False,indent=2)+"\n")
        (folder/"post.md").write_text(f"# {post['title']}\n\nSource: {resolved}\nAuthor: {post['author']}\nSubreddit: {post['subreddit']}\n\n{post['body']}\n")
        errors: list[str] = []
        downloaded = 0
        total = 0
        for url in post["images"][:60]:
            suffix = pathlib.Path(urllib.parse.urlsplit(url).path).suffix.lower()
            if suffix not in (".png",".jpg",".jpeg",".gif",".webp"):
                continue
            filename = media / f"{downloaded+1:03d}{suffix}"
            try:
                size = download_image(session,url,filename,MAX_TOTAL-total)
                total += size
                downloaded += 1
            except ArchiveProblem as exc:
                filename.unlink(missing_ok=True)
                errors.append(str(exc))
        for vid in post["videos"][:4]:
            filename = media / f"{downloaded+1:03d}.mp4"
            try:
                size = download_video(vid,filename,cookie_file,session.headers["User-Agent"])
                if total+size > MAX_TOTAL:
                    raise ArchiveProblem("Archive exceeds 640 MiB")
                total += size
                downloaded += 1
            except ArchiveProblem as exc:
                filename.unlink(missing_ok=True)
                errors.append(str(exc))
        if (post["images"] or post["videos"] or post["declaredPostType"] in {"image","gallery","video"}) and not downloaded:
            raise ArchiveProblem("Could not retrieve post media from HTML; refusing an incomplete archive")
        notes = (f"Source: {resolved}\nCaptured from logged-in web HTML, not Reddit's API.\n"
                 f"Saved media: {downloaded}\nMissing media: {len(errors)}\n"
                 + "\n".join("Media warning: " + e for e in errors) + "\n"
                 + "Some Reddit content is dynamically loaded and may not be discoverable from HTML.\n")
        (folder/"ARCHIVE_NOTES.txt").write_text(notes)
        filename = f"reddit-{post_id}-{sanitize(post['title'])}.tar.zst"
        archive = output_dir / "archive.tar.zst"
        with archive.open("wb") as out:
            tar=subprocess.Popen(["tar","-C",str(output_dir),"-cf","-",folder.name],stdout=subprocess.PIPE)
            assert tar.stdout is not None
            zstd=subprocess.Popen(["zstd","-q","-T0","-3","-c"],stdin=tar.stdout,stdout=out)
            tar.stdout.close()  # Important: let zstd see EOF when tar exits.
            zstd_code=zstd.wait()
            tar_code=tar.wait()
            if tar_code!=0 or zstd_code!=0:
                raise ArchiveProblem("TAR/Zstandard compression failed")
        return {"filename":filename,"mediaCount":str(downloaded),"bytes":str(archive.stat().st_size)}
    finally:
        cookie_file.unlink(missing_ok=True)


def cli() -> None:
    parser=argparse.ArgumentParser()
    parser.add_argument("operation",choices=("running","download","publish"))
    parser.add_argument("job")
    args=parser.parse_args()
    client,bucket=storage()
    output=pathlib.Path(os.getenv("RUNNER_TEMP","/tmp")) / ("gparty-archive-"+args.job)
    output.mkdir(parents=True, exist_ok=True)
    if args.operation=="running":
        set_status(client,bucket,args.job,"running","Runner started. Preparing home-IP download.")
        return
    if args.operation=="download":
        try:
            meta=archive_post(args.job,output)
            (output/"success.json").write_text(json.dumps(meta))
        except Exception as ex:
            message=str(ex) if isinstance(ex,ArchiveProblem) else "Archive extraction failed; check runner logs."
            (output/"failure.txt").write_text(message)
            raise SystemExit(message) from None
        return
    if (output/"success.json").exists() and (output/"archive.tar.zst").exists():
        meta=json.loads((output/"success.json").read_text())
        client.upload_file(str(output/"archive.tar.zst"),bucket,r2key(args.job,"archive.tar.zst"),
            ExtraArgs={"ContentType":"application/zstd","CacheControl":"private, no-store"})
        set_status(client,bucket,args.job,"ready",f"Archive ready ({meta['mediaCount']} media file(s)).",meta["filename"])
    else:
        message=(output/"failure.txt").read_text() if (output/"failure.txt").exists() else "Runner failed before completing the archive."
        set_status(client,bucket,args.job,"failed",message[:220])


if __name__=="__main__":
    cli()
