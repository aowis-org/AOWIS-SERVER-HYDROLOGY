#!/usr/bin/env python3
"""Verify relative Markdown links in the project's public product documentation."""

from pathlib import Path
import re
import unittest
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
LINK = re.compile(r"\]\(([^\s)]+)(?:\s+\"[^\"]+\")?\)")


def broken_links(root=ROOT):
    problems = []
    files = [root / "README.md", *sorted((root / "docs").rglob("*.md"))]
    for file in files:
        text = file.read_text(encoding="utf-8")
        for match in LINK.finditer(text):
            link = match.group(1).strip("<>")
            address = urlsplit(link)
            if address.scheme or address.netloc or not address.path:
                continue
            target = (file.parent / unquote(address.path)).resolve()
            if not target.is_relative_to(root.resolve()) or not target.exists():
                line = text.count("\n", 0, match.start()) + 1
                problems.append(f"{file.relative_to(root)}:{line}: {link}")
    return problems


class DocsLinksTest(unittest.TestCase):
    def test_all_internal_markdown_link_targets_exist(self):
        self.assertEqual(broken_links(), [], "Broken links: " + ", ".join(broken_links()))


if __name__ == "__main__":
    unittest.main()
