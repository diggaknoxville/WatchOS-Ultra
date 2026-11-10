#!/usr/bin/env python3
"""Turn claude-limits.js into the one-line bookmark URL (bookmarklet.txt)."""
import pathlib
import urllib.parse

here = pathlib.Path(__file__).parent
src = here.joinpath("claude-limits.js").read_text(encoding="utf-8")
here.joinpath("bookmarklet.txt").write_text("javascript:" + urllib.parse.quote(src, safe=""), encoding="utf-8")
print("wrote", here / "bookmarklet.txt")
