#!/usr/bin/env python3
"""Validate maintained documentation links and exact screenshot provenance."""
import hashlib
import json
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[2]
LINK = re.compile(r'\[[^\]]*\]\(([^\s)]+)\)|<img\b[^>]*\bsrc="([^"]+)"')


def local_target(root, document, reference):
    parsed = urlsplit(reference)
    if parsed.scheme or parsed.netloc or not parsed.path:
        return None
    path = (document.parent / unquote(parsed.path)).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError('link escapes repository: ' + str(document))
    return path


def jpeg_size(data):
    if not data.startswith(b'\xff\xd8') or not data.endswith(b'\xff\xd9'):
        raise ValueError('invalid JPEG envelope')
    pos = 2
    while pos < len(data):
        if data[pos] != 255:
            raise ValueError('invalid JPEG marker')
        while pos < len(data) and data[pos] == 255:
            pos += 1
        if pos >= len(data):
            break
        marker = data[pos]
        pos += 1
        if marker in (0x01, 0xd8) or 0xd0 <= marker <= 0xd7:
            continue
        length = int.from_bytes(data[pos:pos + 2], 'big')
        if length < 2 or pos + length > len(data):
            raise ValueError('truncated JPEG segment')
        if marker in (0xc0, 0xc1, 0xc2, 0xc3, 0xc5, 0xc6, 0xc7,
                      0xc9, 0xca, 0xcb, 0xcd, 0xce, 0xcf):
            if length < 8:
                raise ValueError('truncated JPEG dimensions')
            return (int.from_bytes(data[pos + 5:pos + 7], 'big'),
                    int.from_bytes(data[pos + 3:pos + 5], 'big'))
        pos += length
    raise ValueError('JPEG dimensions missing')


def check(root=ROOT):
    docs = root / 'labtwin/docs'
    # Original contest template is an archived reference, not maintained docs.
    documents = [root / 'README.md'] + [p for p in docs.glob('*.md')
                                       if p.name != 'CONTEST_TEMPLATE.md']
    for document in documents:
        for match in LINK.finditer(document.read_text(encoding='utf-8')):
            target = local_target(root, document, match.group(1) or match.group(2))
            if target is not None and not target.exists():
                raise ValueError('missing link in {}: {}'.format(document.name, target))
    images = docs / 'images'
    manifest = json.loads((images / 'manifest.json').read_text(encoding='utf-8'))
    entries = manifest['files']
    names = [entry['file'] for entry in entries]
    actual = {p.name for p in images.iterdir() if p.suffix.lower() in ('.jpg', '.jpeg')}
    if len(set(names)) != len(names) or set(names) != actual:
        raise ValueError('screenshot inventory mismatch')
    for entry in entries:
        name = entry['file']
        if Path(name).name != name:
            raise ValueError('invalid screenshot filename')
        data = (images / name).read_bytes()
        if (hashlib.sha256(data).hexdigest() != entry['sha256'] or
                len(data) != entry['bytes'] or
                jpeg_size(data) != (entry['width'], entry['height'])):
            raise ValueError('screenshot content mismatch: ' + name)
        for field in ('date', 'evidence_task', 'firmware_sha_prefix',
                      'portal_revision_prefix', 'scope'):
            if not entry.get(field):
                raise ValueError('screenshot provenance missing: ' + field)
    print('DOCS_CHECK=PASS: {} documents, {} exact screenshots'.format(len(documents), len(entries)))


if __name__ == '__main__':
    check()
