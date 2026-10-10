import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from patch_tai_config import KERNEL, SHELL, TEXT, patch


class TaiConfigTests(unittest.TestCase):
    def test_two_titles_and_idempotence(self):
        raw = b"# existing\r\n*KERNEL\r\nur0:tai/Other.skprx\r\n*PCSG00001\r\nur0:tai/Game.suprx\r\n"
        result = patch(raw, ["PCSG00001", "PCSG00002"], ["ur0:tai/Extra.skprx"])
        self.assertEqual(result, patch(result, ["PCSG00001", "PCSG00002"], ["ur0:tai/Extra.skprx"]))
        self.assertEqual(result.count(TEXT.encode()), 2)
        self.assertIn(b"*PCSG00001\r\n" + TEXT.encode() + b"\r\nur0:tai/Game.suprx", result)
        self.assertNotIn(b"\n", result.replace(b"\r\n", b""))
        self.assertEqual(result.count(KERNEL.encode()), 1)
        self.assertEqual(result.count(SHELL.encode()), 1)

    def test_uninstall_keeps_other_plugins(self):
        result = patch(b"*KERNEL\nur0:tai/Other.skprx\n", ["PCSG00001", "PCSG00002"])
        result = patch(result, uninstall=True)
        self.assertIn(b"ur0:tai/Other.skprx", result)
        for plugin in (KERNEL, SHELL, TEXT):
            self.assertNotIn(plugin.encode(), result)

    def test_existing_global_registration(self):
        result = patch(("*ALL\n" + TEXT + "\n").encode(), ["PCSG00001"])
        self.assertEqual(result.count(TEXT.encode()), 1)

    def test_invalid_title_is_rejected(self):
        for title in ("main", "../config", "PCSG00001\n*KERNEL"):
            with self.assertRaises(ValueError):
                patch(b"", [title])


if __name__ == "__main__":
    unittest.main()
