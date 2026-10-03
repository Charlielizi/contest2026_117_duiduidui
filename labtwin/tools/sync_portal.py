#!/usr/bin/env python3
"""Install a fresh Portal build into the dedicated vendor ROMFS input only."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess


def sync(workspace):
    workspace = workspace.resolve()
    portal = workspace / "tools/labtwin-dashboard"
    dist = portal / "dist-board"
    target = workspace / "vendor/allwinnertech/lichee/board/common/data/res/labtwin-admin"
    if not (workspace / "build.sh").is_file() or not (portal / "package.json").is_file():
        raise ValueError("Apply the source overlay to an openvela checkout first")
    if not target.resolve().is_relative_to(workspace / "vendor/allwinnertech"):
        raise ValueError("ROMFS path escapes vendor checkout")
    subprocess.run(["npm", "run", "build:board"], cwd=portal, check=True)
    index = (dist / "index.html").read_text()
    for required in ("/assets/labtwin-logo.png", "recorder-portal.js?v=", "voice-config-portal.js?v="):
        if required not in index:
            raise ValueError("Missing Portal reference: " + required)
    files = sorted(path for path in dist.rglob("*") if path.is_file())
    if any(path.is_symlink() for path in files):
        raise ValueError("Build output must not contain symlinks")
    names = {path.relative_to(dist).as_posix() for path in files}
    versioned = re.findall(r'(?:src|href)="/assets/([A-Za-z0-9._-]+)\?v=([a-f0-9]{16})"', index)
    if len(versioned) != 5:
        raise ValueError("Expected content versions for all four extensions and favicon")
    for name, version in versioned:
        source = dist / "assets" / name
        data = source.read_bytes()
        if source.suffix in (".js", ".css", ".html"):
            data = data.replace(b"\r\r\n", b"\n").replace(b"\r\n", b"\n")
        if hashlib.sha256(data).hexdigest()[:16] != version:
            raise ValueError("Resource content version mismatch: " + name)
    # This directory contains only generated Portal resources. Remove obsolete
    # hash bundles, not user data or unrelated firmware resources.
    target.mkdir(parents=True, exist_ok=True)
    for pattern in ("index-*.js", "index-*.css", "report-export-*.js", "charts-*.js", "rolldown-runtime-*.js"):
        for old in (target / "assets").glob(pattern):
            if old.is_file() and old.relative_to(target).as_posix() not in names:
                old.unlink()
    checksums = []
    for source in files:
        name = source.relative_to(dist).as_posix()
        data = source.read_bytes()
        if source.suffix in (".js", ".css", ".html"):
            data = data.replace(b"\r\r\n", b"\n").replace(b"\r\n", b"\n")
        dest = target / name
        if not dest.resolve().is_relative_to(target.resolve()):
            raise ValueError("Unsafe ROMFS output path")
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        sha = hashlib.sha256(data).hexdigest()
        if hashlib.sha256(dest.read_bytes()).hexdigest() != sha:
            raise ValueError("ROMFS checksum mismatch")
        checksums.append(sha + "  " + name)
    (target / "RESOURCE_SHA256SUMS").write_text("\n".join(checksums) + "\n")
    # The public snapshot is content-addressed, not a private local commit.
    source_hash = hashlib.sha256((Path(__file__).resolve().parents[1] / "SOURCE_MANIFEST.json").read_bytes()).hexdigest()
    (target / "SOURCE_REVISION").write_text("public-source-manifest-sha256:" + source_hash + "\n")
    print("PORTAL_ROMFS_SYNC=PASS ({} files)".format(len(files)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("workspace", type=Path)
    sync(parser.parse_args().workspace)
