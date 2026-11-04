import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).parents[1] / "runner"))
from archive_reddit import parse_post_html, check_url, ArchiveProblem, sanitize, decode_fragment


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

    def test_decode_and_filename(self):
        self.assertEqual(decode_fragment(r"https:\/\/i.redd.it\/one.png"), "https://i.redd.it/one.png")
        self.assertEqual(sanitize("A / strange:* title!"), "A-strange-title")


if __name__ == "__main__":
    unittest.main()
