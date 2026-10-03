import contextlib
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import apply_overlay
import check_release
import sync_portal


class OverlayTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.release = self.root / 'release'
        self.workspace = self.root / 'workspace'
        self.workspace.mkdir()
        (self.workspace / 'build.sh').touch()
        self.source = self.release / 'overlay/tools/labtwin-dashboard/file.txt'
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(b'public source\n')
        self.entry = {'path': 'tools/labtwin-dashboard/file.txt', 'sha256': hashlib.sha256(self.source.read_bytes()).hexdigest(), 'mode': 0o644}
        self.entries = [self.entry]
        self.repos = [{'path': 'tools/labtwin-dashboard', 'base': None}]
        self.save()

    def save(self):
        (self.release / 'SOURCE_MANIFEST.json').write_text(json.dumps({'repositories': self.repos, 'files': self.entries}))

    def invoke(self, dry=False):
        with contextlib.redirect_stdout(io.StringIO()):
            apply_overlay.apply(self.release, self.workspace, dry)

    def test_dry_run_does_not_write_then_apply_checks_content(self):
        self.invoke(True)
        self.assertFalse((self.workspace / 'tools').exists())
        self.invoke()
        self.assertEqual((self.workspace / self.entry['path']).read_bytes(), b'public source\n')

    def test_checksum_failure_before_write(self):
        self.source.write_bytes(b'corrupted')
        with self.assertRaises(ValueError):
            self.invoke()
        self.assertFalse((self.workspace / 'tools').exists())

    def test_existing_portal_is_not_overwritten(self):
        path = self.workspace / self.entry['path']
        path.parent.mkdir(parents=True)
        path.write_bytes(b'user work')
        with self.assertRaises(ValueError):
            self.invoke()
        self.assertEqual(path.read_bytes(), b'user work')

    def test_relative_symlink_restored(self):
        source = self.source.parent / 'compat.h'
        source.write_bytes(b'file.txt')
        entry = {'path': 'tools/labtwin-dashboard/compat.h', 'sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'mode': 0o644, 'symlink': 'file.txt'}
        self.entries.append(entry)
        self.save()
        self.invoke()
        path = self.workspace / entry['path']
        self.assertTrue(path.is_symlink())
        self.assertEqual(path.read_bytes(), b'public source\n')

    def test_traversal_and_external_symlink_rejected(self):
        self.entry['path'] = '../escape.txt'
        self.save()
        with self.assertRaises(ValueError):
            self.invoke()

    def test_dirty_or_wrong_baseline_rejected(self):
        self.repos = [{'path': 'apps', 'base': 'a' * 40}]
        self.save()
        for values in (['b' * 40, ''], ['a' * 40, ' M file.c']):
            with patch('apply_overlay.subprocess.check_output', side_effect=values):
                with self.assertRaises(ValueError):
                    self.invoke()


class ReleaseTests(unittest.TestCase):
    def test_jpeg_allowed_only_for_documentation_images(self):
        jpeg = b'\xff\xd8\xff\0screenshot\xff\xd9'
        self.assertFalse(check_release.violations('labtwin/docs/images/demo.jpg', jpeg))
        self.assertIn('unapproved-binary', check_release.violations('private/demo.jpg', jpeg))
        self.assertIn('unapproved-binary', check_release.violations('labtwin/docs/images/demo.jpg', b'\0not-a-jpeg'))

    def test_jpeg_still_scanned_for_secret_literals(self):
        jpeg = b'\xff\xd8\xff\0' + ('sk-' + 'A' * 24).encode() + b'\xff\xd9'
        self.assertIn('provider-key', check_release.violations('labtwin/docs/images/demo.jpg', jpeg))

    def test_secrets_and_private_paths_detected_without_values(self):
        samples = [
            ('asset.img', b'not firmware'),
            ('voice.tflite', b'model'),
            ('config', ('CONFIG_AI_AGENT_PRIVATE_OPEN_ADMIN=' + 'y').encode()),
            ('config', ('CONFIG_AI_AGENT_DEFAULT_WIFI_PASSWORD=' + '"not-public"').encode()),
            ('source', ('sk-' + 'A' * 24).encode()),
            ('source', ('-----BEGIN ' + 'PRIVATE KEY-----').encode()),
        ]
        for name, data in samples:
            self.assertTrue(check_release.violations(name, data))

    def test_untrained_model_required(self):
        name = 'src/voice/wakeword_model_data.cc'
        self.assertTrue(check_release.violations(name, b'private model'))
        self.assertFalse(check_release.violations(name, b'g_openvela_wakeword_model_len = 0; "untrained"'))


class SyncTests(unittest.TestCase):
    def test_versions_and_content_then_sync_only_dedicated_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'build.sh').touch()
            portal = root / 'tools/labtwin-dashboard'
            assets = portal / 'dist-board/assets'
            assets.mkdir(parents=True)
            (portal / 'package.json').write_text('{}')
            names = ['labtwin-logo.png', 'recorder-portal.css', 'voice-config-portal.css', 'recorder-portal.js', 'voice-config-portal.js']
            html = []
            for name in names:
                value = b'bytes\n'
                (assets / name).write_bytes(value)
                html.append('<script src="/assets/{}?v={}"></script>'.format(name, hashlib.sha256(value).hexdigest()[:16]))
            (portal / 'dist-board/index.html').write_text('\n'.join(html))
            target = root / 'vendor/allwinnertech/lichee/board/common/data/res/labtwin-admin'
            (target / 'assets').mkdir(parents=True)
            (target / 'assets/index-old.js').write_text('obsolete generated bundle')
            (target / 'keep.txt').write_text('unrelated')
            with patch('sync_portal.subprocess.run'), contextlib.redirect_stdout(io.StringIO()):
                sync_portal.sync(root)
            self.assertFalse((target / 'assets/index-old.js').exists())
            self.assertEqual((target / 'keep.txt').read_text(), 'unrelated')
            for row in (target / 'RESOURCE_SHA256SUMS').read_text().splitlines():
                sha, name = row.split('  ', 1)
                self.assertEqual(hashlib.sha256((target / name).read_bytes()).hexdigest(), sha)
            (assets / 'recorder-portal.js').write_text('tampered')
            with patch('sync_portal.subprocess.run'):
                with self.assertRaises(ValueError):
                    sync_portal.sync(root)


if __name__ == '__main__':
    unittest.main()
