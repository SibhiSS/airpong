#!/usr/bin/env python3
"""
Prefetch the esp32 Arduino core's tool archives with resumable downloads.

Why this exists: `arduino-cli core install` downloads each archive in one shot
with no resume. On a connection that drops mid-transfer, a large archive can
fail forever — it restarts from zero every time. This fetches the same archives
with `curl -C -` (true HTTP range resume) plus retries, verifies the SHA-256
that arduino-cli would verify, and drops them into arduino-cli's staging cache.

`arduino-cli core install` then finds them already present, checksums them, and
skips straight to extraction.

Usage:  python tools/prefetch_esp32_core.py [version]     (default 2.0.17)
"""
import hashlib
import json
import os
import subprocess
import sys

VERSION = sys.argv[1] if len(sys.argv) > 1 else "2.0.17"
A15 = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "Arduino15")
INDEX = os.path.join(A15, "package_esp32_index.json")
STAGING = os.path.join(A15, "staging", "packages")


def pick_system(systems):
    """Mirror arduino-cli's host selection on 64-bit Windows.

    It prefers an exact arch match and falls back to the 32-bit build, which is
    what most of the older ESP32 toolchains actually ship.
    """
    for pref in ("x86_64-mingw32", "x86_64-w64-mingw32", "i686-mingw32", "i686-w64-mingw32"):
        for s in systems:
            if s["host"] == pref:
                return s
    return None


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    if not os.path.exists(INDEX):
        sys.exit(f"index not found: {INDEX}\nrun: arduino-cli core update-index")

    with open(INDEX, encoding="utf-8") as f:
        pkg = [p for p in json.load(f)["packages"] if p["name"] == "esp32"][0]

    tools = {(t["name"], t["version"]): t for t in pkg["tools"]}
    plat = [p for p in pkg["platforms"] if p["version"] == VERSION]
    if not plat:
        have = sorted({p["version"] for p in pkg["platforms"]}, reverse=True)[:8]
        sys.exit(f"esp32 {VERSION} not in index. Recent: {', '.join(have)}")
    plat = plat[0]

    targets = []
    # The platform archive itself, plus every tool it depends on.
    targets.append((plat["archiveFileName"], plat["url"], int(plat["size"]), plat["checksum"]))
    for dep in plat["toolsDependencies"]:
        tool = tools.get((dep["name"], dep["version"]))
        if not tool:
            continue
        s = pick_system(tool.get("systems", []))
        if s:
            targets.append((s["archiveFileName"], s["url"], int(s["size"]), s["checksum"]))

    os.makedirs(STAGING, exist_ok=True)
    total = sum(t[2] for t in targets)
    print(f"esp32:esp32@{VERSION} — {len(targets)} archives, {total/1e6:.0f} MB total")
    print(f"staging: {STAGING}\n")

    failed = []
    for i, (name, url, size, checksum) in enumerate(targets, 1):
        path = os.path.join(STAGING, name)
        algo, _, want = checksum.partition(":")

        if os.path.exists(path) and os.path.getsize(path) == size:
            if algo == "SHA-256" and sha256(path) == want.lower():
                print(f"[{i}/{len(targets)}] {name}  already complete, verified")
                continue
            print(f"[{i}/{len(targets)}] {name}  bad checksum, refetching")
            os.remove(path)

        have = os.path.getsize(path) if os.path.exists(path) else 0
        print(f"[{i}/{len(targets)}] {name}  {size/1e6:.0f} MB"
              + (f" (resuming from {have/1e6:.0f} MB)" if have else ""))

        # -C - resumes from wherever the previous attempt died. The retry flags
        # cover transient drops without restarting the whole transfer.
        rc = subprocess.call([
            "curl", "-L", "-C", "-", "--retry", "20", "--retry-delay", "3",
            "--retry-all-errors", "--connect-timeout", "30",
            "--speed-time", "60", "--speed-limit", "1024",
            "-o", path, url,
        ])

        if rc != 0 or not os.path.exists(path) or os.path.getsize(path) != size:
            got = os.path.getsize(path) if os.path.exists(path) else 0
            print(f"    INCOMPLETE ({got/1e6:.0f}/{size/1e6:.0f} MB) — rerun to resume")
            failed.append(name)
            continue

        if algo == "SHA-256" and sha256(path) != want.lower():
            print("    CHECKSUM MISMATCH — deleting, rerun to refetch")
            os.remove(path)
            failed.append(name)
            continue
        print("    ok, verified")

    print()
    if failed:
        print(f"{len(failed)} incomplete: {', '.join(failed)}")
        print("Rerun this script — completed files are skipped and partials resume.")
        return 1
    print("All archives present and verified.")
    print(f"Now run:  arduino-cli core install esp32:esp32@{VERSION}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
