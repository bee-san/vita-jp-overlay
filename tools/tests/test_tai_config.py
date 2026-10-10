import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from patch_tai_config import KERNEL, SHELL, TEXT, OCR, patch

class TaiConfigTests(unittest.TestCase):
    def test_ocr_only_and_idempotence(self):
        raw = ("# existing\r\n*KERNEL\r\nur0:tai/Other.skprx\r\n*PCSG00415\r\n"
               + TEXT + "\r\n" + OCR + "\r\nur0:tai/Game.suprx\r\n").encode()
        result = patch(raw, ["ur0:tai/Extra.skprx"])
        self.assertEqual(result, patch(result, ["ur0:tai/Extra.skprx"]))
        active = [line.strip() for line in result.decode().splitlines() if not line.startswith("#")]
        self.assertNotIn(TEXT, active)
        self.assertNotIn(OCR, active)
        self.assertIn("ur0:tai/Game.suprx", active)
        self.assertIn("ur0:tai/Other.skprx", active)
        self.assertNotIn(b"\n", result.replace(b"\r\n", b""))
        self.assertEqual(active.count(KERNEL), 1)
        self.assertEqual(active.count(SHELL), 1)

    def test_uninstall_keeps_other_plugins(self):
        raw = ("*KERNEL\n" + KERNEL + "\nur0:tai/Other.skprx\n*ALL\n" + TEXT
               + "\n" + OCR + "\n*main\n" + SHELL + "\n").encode()
        result = patch(raw, uninstall=True)
        self.assertIn(b"ur0:tai/Other.skprx", result)
        for plugin in (KERNEL, SHELL, TEXT, OCR):
            self.assertNotIn(plugin.encode(), result)

    def test_existing_global_registration_is_disabled(self):
        result = patch(("*ALL\n" + TEXT + "\n").encode())
        self.assertIn(("# Lens-only: " + TEXT).encode(), result)
        self.assertEqual(result.count(TEXT.encode()), 1)

    def test_repeated_sections_preserve_existing_registrations(self):
        raw = ("*KERNEL\nur0:tai/Other.skprx\n*main\nur0:tai/Other.suprx\n"
               "*KERNEL\n" + KERNEL + "\n*main\n" + SHELL + "\n"
               "*PCSG00415\n" + OCR + "\n").encode()
        expected = raw.replace((OCR + "\n").encode(), ("# Lens-only: " + OCR + "\n").encode())
        self.assertEqual(patch(raw), expected)
        self.assertEqual(patch(expected), expected)

if __name__ == "__main__":
    unittest.main()
