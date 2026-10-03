#!/usr/bin/env python3
"""Check the exact Git file list, content inventory and newly added history."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
FORBIDDEN = re.compile(r"(?:^|/)(?:node_modules|recordings|private-state|firmware_history|__pycache__|\.built|\.depend|\.env[^/]*|\.config(?:\.[^/]*)?)(?:/|$)|\.(?:img|dpapi|m4a|wav|part|tflite|pyc|p12|pfx|key|bundle|rpk|elf|o|a)$", re.I)
PATTERNS = {
    "private-key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "provider-key": re.compile(r"\b(?:sk-[A-Za-z0-9_-]{20,}|AKIA[A-Z0-9]{16})\b"),
    "default-wifi": re.compile(r'^CONFIG_AI_AGENT_DEFAULT_WIFI_(?:SSID|PASSWORD)="([^"\n]+)"', re.M),
    "open-admin": re.compile(r"^CONFIG_AI_AGENT_PRIVATE_OPEN_ADMIN=y$", re.M),
    "credential-literal": re.compile(r'(?:api[_-]?key|access[_-]?key|secret[_-]?key|auth[_-]?token|wifi[_-]?password)\s*[=:]\s*[\x22\x27]([^\x22\x27\n]{16,})[\x22\x27]', re.I),
}


def names():
    data = subprocess.check_output(["git", "-C", str(ROOT), "ls-files", "--cached", "--others", "--exclude-standard", "-z"])
    return sorted(set(filter(None, data.decode().split("\0"))))


def violations(name, data):
    errors = []
    if FORBIDDEN.search(name):
        errors.append("forbidden-path")
    text = data.decode("utf-8", errors="replace")
    approved_png = name.endswith('.png') and data.startswith(b'\x89PNG\r\n\x1a\n')
    approved_doc_jpeg = (name.startswith('labtwin/docs/images/') and
                         name.endswith(('.jpg', '.jpeg')) and
                         data.startswith(b'\xff\xd8\xff') and data.endswith(b'\xff\xd9'))
    if b'\0' in data and not (approved_png or approved_doc_jpeg):
        errors.append('unapproved-binary')
    for rule, pattern in PATTERNS.items():
        for match in pattern.finditer(text):
            value = match.group(1) if match.lastindex else match.group(0)
            if re.fullmatch(r"(?:sk-)?[xX_-]+", value) or "${" in value or value.startswith("process.env."):
                continue
            errors.append(rule)
            break
    if name.endswith("src/voice/wakeword_model_data.cc"):
        if 'g_openvela_wakeword_model_len = 0' not in text or '"untrained"' not in text:
            errors.append("nonpublic-trained-model")
    return errors


def check(write_inventory=False, base=None):
    files = names()
    errors = []
    for name in files:
        path = ROOT / name
        if not path.is_file() or path.is_symlink():
            errors.append((name, "missing-or-symlink"))
            continue
        errors.extend((name, rule) for rule in violations(name, path.read_bytes()))
    manifest = json.loads((ROOT / "labtwin/SOURCE_MANIFEST.json").read_text())
    expected = {entry["path"] for entry in manifest["files"]}
    actual = {name.removeprefix("labtwin/overlay/") for name in files if name.startswith("labtwin/overlay/")}
    if expected != actual:
        errors.append(("SOURCE_MANIFEST.json", "overlay-file-list-mismatch"))
    for entry in manifest["files"]:
        path = ROOT / "labtwin/overlay" / entry["path"]
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            errors.append((entry["path"], "overlay-checksum-mismatch"))
    inventory = "labtwin/RELEASE_SHA256SUMS"
    rows = [hashlib.sha256((ROOT / name).read_bytes()).hexdigest() + "  " + name for name in files if name != inventory and (ROOT / name).is_file()]
    content = "\n".join(rows) + "\n"
    if write_inventory:
        (ROOT / inventory).write_text(content, encoding="utf-8", newline="\n")
    elif not (ROOT / inventory).exists() or (ROOT / inventory).read_text() != content:
        errors.append((inventory, "release-inventory-mismatch"))
    commits = []
    if base:
        commits = subprocess.check_output(["git", "-C", str(ROOT), "rev-list", "HEAD", "^" + base], text=True).splitlines()
        for commit in commits:
            changed = subprocess.check_output(["git", "-C", str(ROOT), "diff-tree", "--no-commit-id", "--diff-filter=AM", "--name-only", "-r", "-z", commit]).decode().split("\0")
            for name in filter(None, changed):
                data = subprocess.check_output(["git", "-C", str(ROOT), "show", commit + ":" + name])
                errors.extend((commit[:8] + "/" + name, rule) for rule in violations(name, data))
    for path, rule in errors:
        print("FAIL {}: {}".format(rule, path))
    if errors:
        raise SystemExit(1)
    print("RELEASE_CHECK=PASS: {} files, {} new commits, full text scanned (no size cutoff)".format(len(rows) + 1, len(commits)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write-inventory", action="store_true")
    parser.add_argument("--base", help="Scan only new history above the already-public base")
    args = parser.parse_args()
    check(args.write_inventory, args.base)
