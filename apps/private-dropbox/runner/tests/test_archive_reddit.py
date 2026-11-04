import pathlib
import sys
import unittest
import base64
import io
import json
import os
import subprocess
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).parents[1]))
from archive_reddit import parse_post_html, check_url, ArchiveProblem, sanitize, decode_fragment, archive_post


class ArchiveTests(unittest.TestCase):
    def test_html_gallery_excludes_recommended_posts(self):
        page = """<html><head><meta property="og:title" content="Fallback"></head><body>
        <shreddit-post author="user123" subreddit-name="r/example" post-type="gallery" post-title="My Photos!">
          <div slot="text-body">hello &amp; good day</div>
          <shreddit-gallery><a href="https://i.redd.it/one.jpg">one</a>
          <img src="https://preview.redd.it/two.png?width=600&amp;format=png" /></shreddit-gallery>
        </shreddit-post>
        <shreddit-post post-title="Recommended">
          <img src="https://i.redd.it/not-mine.jpg"></shreddit-post>
        </body></html>"""
        post = parse_post_html(page, "https://www.reddit.com/r/example/comments/abc123/")
        self.assertEqual(post["title"], "My Photos!")
        self.assertEqual(post["author"], "user123")
        self.assertEqual(post["body"], "hello & good day")
        self.assertEqual(len(post["images"]), 2)
        self.assertTrue(all("not-mine" not in url for url in post["images"]))

    def test_video_direct_manifest_id(self):
        page = """<shreddit-post post-title="Movie" post-type="video">
          <video src="https://v.redd.it/Abc1234/DASH_720.mp4"></video></shreddit-post>"""
        self.assertEqual(parse_post_html(page, "https://www.reddit.com/comments/abc123/")["videos"], ["Abc1234"])

    def test_url_safety(self):
        for target in ("https://localhost/path", "http://www.reddit.com/comments/abc123/",
            "https://reddit.com.evil.test/r/test/", "https://127.0.0.1/path"):
            with self.assertRaises(ArchiveProblem):
                check_url(target)
        with self.assertRaises(ArchiveProblem):
            check_url("https://127.0.0.1/x", media=True)

    def test_real_tar_zstd_archive_from_mock_logged_in_post(self):
        job = "123e4567-e89b-42d3-a456-426614174000"
        post_url = "https://www.reddit.com/r/test/comments/abc123/title/"
        page = """<shreddit-post post-title="Archive Test" author="demo"
            subreddit-name="r/test" post-type="image">
            <img src="https://i.redd.it/one.png"></shreddit-post>"""
        cookie = ("# Netscape HTTP Cookie File\n"
                  ".reddit.com\tTRUE\t/\tTRUE\t2147483647\treddit_session\tfake-test-only\n")
        class FakeR2:
            def get_object(self, **kwargs):
                self_key = kwargs["Key"]
                assert self_key.endswith("/request.json")
                return {"Body": io.BytesIO(json.dumps({"url": post_url}).encode())}
        def save_image(_session, _url, target, _budget):
            target.write_bytes(bytes([137, 80, 78, 71, 1, 2, 3]))
            return target.stat().st_size
        with tempfile.TemporaryDirectory() as tmp:
            with patch.dict(os.environ, {
                "GPARTY_SOURCE_ROUTE_VERIFIED": "1",
                "REDDIT_COOKIES_BASE64": base64.b64encode(cookie.encode()).decode(),
                "REDDIT_BROWSER_UA": "",
            }):
                with patch("archive_reddit.storage", return_value=(FakeR2(), "private-test")), \
                     patch("archive_reddit.get_page", return_value=(page, post_url)), \
                     patch("archive_reddit.download_image", side_effect=save_image):
                    meta = archive_post(job, pathlib.Path(tmp))
            self.assertEqual(meta["mediaCount"], "1")
            archive = pathlib.Path(tmp) / "archive.tar.zst"
            self.assertTrue(archive.is_file())
            listing = subprocess.run(
                ["tar", "-I", "zstd", "-tf", str(archive)],
                check=True, capture_output=True, text=True, timeout=20,
            ).stdout
            self.assertIn("reddit-abc123/post.json", listing)
            self.assertIn("reddit-abc123/post.md", listing)
            self.assertIn("reddit-abc123/media/001.png", listing)
            self.assertIn("reddit-abc123/ARCHIVE_NOTES.txt", listing)
            self.assertFalse((pathlib.Path(tmp) / "cookies.txt").exists())

    def test_decode_and_filename(self):
        self.assertEqual(decode_fragment(r"https:\/\/i.redd.it\/one.png"), "https://i.redd.it/one.png")
        self.assertEqual(sanitize("A / strange:* title!"), "A-strange-title")


if __name__ == "__main__":
    unittest.main()
