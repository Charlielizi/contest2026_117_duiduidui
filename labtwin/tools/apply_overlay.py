#!/usr/bin/env python3
"""Apply reviewed source files to a clean, exact upstream openvela baseline."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def within(root, name):
    path = root / name
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError("Path escapes checkout: " + name)
    return path


def apply(release, workspace, dry_run=False):
    release, workspace = release.resolve(), workspace.resolve()
    manifest = json.loads((release / "SOURCE_MANIFEST.json").read_text())
    if not (workspace / "build.sh").is_file():
        raise ValueError("Not an openvela root (missing build.sh)")
    for repo in manifest["repositories"]:
        path = within(workspace, repo["path"])
        if repo["base"]:
            head = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
            status = subprocess.check_output(["git", "-C", str(path), "status", "--porcelain", "--untracked-files=no"], text=True)
            if head != repo["base"] or status:
                raise ValueError("Expected a clean pinned baseline: " + repo["path"])
        elif path.exists() and any(path.iterdir()):
            raise ValueError("Existing Portal directory would be overwritten: " + repo["path"])
    # Validate every input before the first write. No private Git history is used.
    for entry in manifest["files"]:
        source = within(release / "overlay", entry["path"])
        if source.is_symlink() or digest(source) != entry["sha256"]:
            raise ValueError("Source checksum/type mismatch: " + entry["path"])
        dest = within(workspace, entry["path"])
        if dest.is_symlink():
            # Only replace a baseline symlink if its exact path remains inside
            # this checkout; never write through it into another location.
            within(workspace, entry["path"])
        if "symlink" in entry:
            if not (dest.parent / entry["symlink"]).resolve().is_relative_to(workspace):
                raise ValueError("Unsafe symlink: " + entry["path"])
    if not dry_run:
        for entry in manifest["files"]:
            source = release / "overlay" / entry["path"]
            dest = workspace / entry["path"]
            dest.parent.mkdir(parents=True, exist_ok=True)
            if dest.is_symlink():
                dest.unlink()
            if "symlink" in entry:
                if dest.exists():
                    dest.unlink()
                dest.symlink_to(entry["symlink"])
            else:
                dest.write_bytes(source.read_bytes())
                dest.chmod(entry["mode"])
        for entry in manifest["files"]:
            dest = workspace / entry["path"]
            if "symlink" in entry:
                if not dest.is_symlink() or str(dest.readlink()) != entry["symlink"]:
                    raise ValueError("Symlink verification failed: " + entry["path"])
            elif digest(dest) != entry["sha256"]:
                raise ValueError("Destination checksum mismatch: " + entry["path"])
    print("OVERLAY {}: {} files / {} repositories".format("CHECKED" if dry_run else "APPLIED", len(manifest["files"]), len(manifest["repositories"])))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("workspace", type=Path)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    apply(Path(__file__).resolve().parents[1], args.workspace, args.dry_run)
