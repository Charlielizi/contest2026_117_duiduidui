from pathlib import Path
import tempfile
import unittest

import check_docs


class DocumentationTests(unittest.TestCase):
    def test_external_and_fragment_links_do_not_need_local_files(self):
        root = Path(tempfile.gettempdir())
        document = root / 'README.md'
        self.assertIsNone(check_docs.local_target(root, document, 'https://example.org/demo'))
        self.assertIsNone(check_docs.local_target(root, document, '#demo'))

    def test_local_links_remain_inside_repository(self):
        root = Path(tempfile.gettempdir()) / 'docs-test'
        document = root / 'README.md'
        self.assertEqual(check_docs.local_target(root, document, 'labtwin/docs/DEMO.md'),
                         (root / 'labtwin/docs/DEMO.md').resolve())
        with self.assertRaises(ValueError):
            check_docs.local_target(root, document, '../private.md')

    def test_jpeg_dimensions_and_truncation(self):
        jpeg = b'\xff\xd8\xff\xc0\x00\x08\x08\x00\x64\x00\xc8\x01\xff\xd9'
        self.assertEqual(check_docs.jpeg_size(jpeg), (200, 100))
        with self.assertRaises(ValueError):
            check_docs.jpeg_size(b'\xff\xd8\xff\xc0\x00\x08\xff\xd9')


if __name__ == '__main__':
    unittest.main()
