#!/usr/bin/env python3
"""Embed web/game.html and tools/scope.html into firmware headers as PROGMEM
strings, so the ESP32 can serve both itself — no LittleFS needed at this size.
Rerun after editing either page:  python tools/embed_pages.py
"""
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
FW = ROOT / "firmware" / "air_tt_paddle"

PAGES = [
    (ROOT / "web" / "game.html", FW / "game_page.h", "GAME_HTML", "AIRTTGAME"),
    (ROOT / "tools" / "scope.html", FW / "scope_page.h", "SCOPE_HTML", "AIRTTSCOPE"),
]

for src, dst, symbol, delim in PAGES:
    html = src.read_text(encoding="utf-8")
    assert f"){delim}\"" not in html, f"delimiter collision in {src}, pick a new one"
    dst.write_text(
        f"// Auto-generated from {src.relative_to(ROOT).as_posix()} by tools/embed_pages.py — do not hand-edit.\n"
        f"// Regenerate after editing that file:  python tools/embed_pages.py\n"
        "#pragma once\n"
        "#include <Arduino.h>   // for PROGMEM — must precede its use regardless of include order elsewhere\n\n"
        f'static const char {symbol}[] PROGMEM = R"{delim}(\n{html}\n){delim}";\n',
        encoding="utf-8",
    )
    print(f"embedded {len(html)} bytes  {src.relative_to(ROOT)} -> {dst.relative_to(ROOT)}")
