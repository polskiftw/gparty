from __future__ import annotations

import re
import unittest
from pathlib import Path


WEB_DIR = Path(__file__).resolve().parents[1]
SRC_DIR = WEB_DIR / "src"


class ViewerLayoutHardeningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.css = (SRC_DIR / "style.css").read_text(encoding="utf-8")
        cls.script = (SRC_DIR / "app.js").read_text(encoding="utf-8")
        cls.viewer = (SRC_DIR / "viewer.js").read_text(encoding="utf-8")

    def mobile_rule(self, selector: str) -> str:
        mobile_css = self.css.split("@media (max-width: 700px)", 1)[1]
        match = re.search(
            rf"{re.escape(selector)}\s*\{{(?P<body>.*?)\}}",
            mobile_css,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(match, f"Missing mobile rule for {selector}")
        return match.group("body")

    def test_mobile_media_uses_intrinsic_box_instead_of_object_fit_canvas(self) -> None:
        media_rule = self.mobile_rule("#media")
        self.assertIn("width: auto !important", media_rule)
        self.assertIn("height: auto !important", media_rule)
        self.assertIn("max-width: 100%", media_rule)
        self.assertIn("max-height: 100%", media_rule)
        self.assertIn("object-fit: initial", media_rule)
        self.assertNotIn("width: 100% !important", media_rule)
        self.assertNotIn("height: 100% !important", media_rule)

    def test_mobile_wrapper_keeps_media_off_fractional_clip_edges(self) -> None:
        wrapper_rule = self.mobile_rule("#media-wrap")
        self.assertIn("padding-block: 1px", wrapper_rule)
        self.assertIn("overflow: visible", wrapper_rule)

    def test_dynamic_viewport_height_is_used_when_supported(self) -> None:
        self.assertIn("@supports (height: 100dvh)", self.css)
        self.assertRegex(
            self.css,
            r"html,\s*body\s*\{\s*height:\s*100dvh;\s*\}",
        )

    def test_mobile_javascript_removes_desktop_pixel_dimensions(self) -> None:
        self.assertIn('media.style.removeProperty("width")', self.script)
        self.assertIn('media.style.removeProperty("height")', self.script)
        self.assertNotIn('media.style.width = "100%"', self.script)
        self.assertNotIn('media.style.height = "100%"', self.script)

    def test_safari_viewport_and_stage_changes_refresh_sizing(self) -> None:
        self.assertIn("window.visualViewport", self.script)
        self.assertIn('window.addEventListener("orientationchange"', self.script)
        self.assertIn("new ResizeObserver(scheduleSizeRefresh).observe(stage)", self.script)
        self.assertIn('window.addEventListener("pageshow"', self.script)

    def test_viewer_assets_revalidate_after_deployment(self) -> None:
        asset_response = self.viewer.split("function assetResponse", 1)[1].split(
            "function jsonResponse",
            1,
        )[0]
        self.assertIn('"cache-control": "no-cache"', asset_response)
        self.assertNotIn("max-age", asset_response)

    def test_tag_sidebar_is_desktop_only(self) -> None:
        self.assertIn("#tag-sidebar {", self.css)
        self.assertIn("#tag-sidebar { display: none; }", self.css)
        self.assertIn('id="tag-sidebar"', self.viewer)
        self.assertIn("if (!mobileQuery.matches) loadTagCatalog()", self.script)

    def test_selected_tags_use_server_side_and_matching(self) -> None:
        self.assertIn('url.searchParams.getAll("tag")', self.viewer)
        self.assertIn("sortedIds.every((id) => item.tags.includes(id))", self.viewer)
        self.assertIn('parameters.append("tag", tag)', self.script)
        self.assertIn("checkbox.value = entry.name", self.script)

    def test_impossible_tag_combination_has_plain_empty_state(self) -> None:
        self.assertIn('noMatch.textContent = "Nothing matches those tags."', self.script)
        self.assertIn('#no-match {', self.css)


if __name__ == "__main__":
    unittest.main()
