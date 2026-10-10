"""Real converter -> on-disk format -> C runtime -> overlay integration tests."""
import contextlib
import ftplib
import io
import json
import os
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import convert_dictionary as convert
import gen_deinflect
import install_dictionary as install

CLI = Path(os.environ.get("VJO_CLI", ROOT / "build/host/vjo-cli"))


def term(word, reading="", gloss=None, rules="", score=1):
    return [word, reading, "", rules, score, gloss or ["definition of " + word], 1, ""]


class LocalDictionaryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def archive(self, rows, name="input.zip", title="Test"):
        path = self.directory / name
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr("index.json", json.dumps({"format": 3, "title": title}))
            z.writestr("term_bank_1.json", json.dumps(rows, ensure_ascii=False))
        return path

    def build(self, rows, name="main.vjdict", title="Test", format_version=2):
        path = self.directory / name
        stats = convert.convert([self.archive(rows, title=title)], path, format_version=format_version)
        return path, stats

    def lookup(self, text, names="main.vjdict", ok=True):
        result = subprocess.run([str(CLI), "--dict", "local", "--local-dir", str(self.directory),
                                 "--local-dicts", names, "--text", text, "--nav", "--stats"],
                                capture_output=True, text=True, timeout=15)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertGreater(result.returncode, 0, "runtime crashed: " + result.stderr)
        return result

    def test_inflection_reading_structured_content_and_highlights(self):
        self.build([term("食べる", "たべる", ["to eat"], "v1"),
                    term("猫", "ねこ", [{"type": "structured-content", "content": [
                        {"tag": "div", "content": ["cat", {"tag": "br"}, "feline"]},
                        {"tag": "img", "alt": "cat illustration"}]}]),
                    term("行く", "いく", ["go"], "v5"), term("高い", "たかい", ["expensive"], "adj-i"),
                    term("する", "する", ["do"], "vs"), term("来る", "くる", ["come"], "vk")])
        result = self.lookup("😀猫は食べたくなかった。高くない。行った。しました。来ました。ネコ。")
        for expected in ("【猫】", "【食べたくなかった】", "【高くない】", "【行った】", "【しました】", "【来ました】",
                         "cat\nfeline", "cat illustration"):
            self.assertIn(expected, result.stdout)
        self.assertIn("猫 (ねこ)", self.lookup("ネコ").stdout)
        self.assertIn("【猫】", self.lookup("😀\n猫\nを見た").stdout)

    def test_longest_match_and_rule_filter(self):
        self.build([term("猫"), term("猫舌", "ねこじた", ["sensitive to hot food"]),
                    term("食べる", "たべる", ["wrong part of speech"], ""),
                    term("食べる", "たべる", ["correct verb"], "v1")])
        result = self.lookup("猫舌。食べました。")
        self.assertIn("【猫舌】", result.stdout)
        self.assertNotIn("【猫】", result.stdout)
        self.assertIn("correct verb", result.stdout)
        self.assertNotIn("wrong part of speech", result.stdout)

    def test_multiple_dictionaries_and_priority(self):
        self.build([term("猫", "ねこ", ["first dictionary"])], title="First")
        self.build([term("猫", "ねこ", ["second dictionary"])], name="extra.vjdict", title="Second")
        output = self.lookup("猫", "main.vjdict,extra.vjdict").stdout
        self.assertIn("[First]", output)
        self.assertIn("[Second]", output)
        self.assertLess(output.index("first dictionary"), output.index("second dictionary"))

    def test_combined_archive_and_html(self):
        one = self.archive([term("猫", gloss=["<p>cat &amp; kitten</p><script>hidden()</script>"])], "one.zip", "One")
        two = self.archive([term("猫", gloss=["second"])], "two.zip", "Two")
        stats = convert.convert([one, two], self.directory / "main.vjdict")
        self.assertEqual(stats["entries"], 2)
        output = self.lookup("猫").stdout
        self.assertIn("cat & kitten", output)
        self.assertNotIn("hidden()", output)
        self.assertIn("[Two]", output)

    def test_bilingual_structured_content_and_empty_image_labels(self):
        self.build([term("猫", "ねこ", [{"type": "structured-content", "content": [
            {"tag": "div", "content": ["cat; a feline", {"tag": "br"}, "猫科の動物。"]},
            {"tag": "img", "title": ""},
            {"type": "image", "alt": "", "title": "diagram caption"},
            {"tag": "img", "alt": "cat illustration", "title": "unused title"},
        ]}, '<p>English 日本語</p><img alt=""><img alt title="HTML caption">'])])
        output = self.lookup("猫").stdout
        for expected in ("cat; a feline\n猫科の動物。", "English 日本語", "diagram caption",
                         "cat illustration", "HTML caption"):
            self.assertIn(expected, output)
        self.assertEqual(output.count("[image omitted]"), 2)
        self.assertNotIn("unused title", output)

    def test_reading_only_term_uses_reading_as_display_headword(self):
        _, stats = self.build([term("", "あて", ["a target; 目標。"]),
                               term("猫", "ねこ", ["cat"])])
        self.assertEqual(stats["entries"], 2)
        self.assertEqual(stats["reading_only_entries"], 1)
        self.assertEqual(stats["index_records"], 3)
        output = self.lookup("あて").stdout
        self.assertIn("【あて】", output)
        self.assertIn("a target; 目標。", output)
        _, stats = self.build([term("", "", ["no usable key"]), term("猫")])
        self.assertEqual(stats["entries"], 1)
        self.assertEqual(stats["skipped_empty_headwords"], 1)
        with self.assertRaisesRegex(ValueError, "no installable term entries"):
            self.build([term("", "", ["no usable key"])])

    def test_missing_partial_and_malformed_files(self):
        self.assertIn("Cannot read local dictionaries", self.lookup("猫", ok=False).stdout)
        path, _ = self.build([term("猫")], format_version=1)
        original = path.read_bytes()
        for change in (b"bad", original[:-1], b"WRONG!!!" + original[8:],
                       original[:16] + struct.pack("<I", 0xFFFFFFFF) + original[20:]):
            path.write_bytes(change)
            self.assertIn("local dictionary", self.lookup("猫", ok=False).stdout)
        # A well-sized header with a record pointing outside the file is rejected.
        bad = bytearray(original)
        struct.pack_into("<Q", bad, convert.HEADER.size, 0xFFFFFFFFFFFFFFFF)
        path.write_bytes(bad)
        self.assertIn("Invalid or incomplete", self.lookup("猫", ok=False).stdout)

    def test_config_does_not_allow_traversal_or_unbounded_files(self):
        self.build([term("猫")])
        for names in ("../main.vjdict", "main.vjdict,", "main.vjdict,,main.vjdict", "a/b.vjdict",
                      ",".join(["main.vjdict"] * 9)):
            self.assertIn("Cannot read local dictionaries", self.lookup("猫", names, ok=False).stdout)
        # A missing second file must not silently appear as a successful partial lookup.
        self.assertIn("Cannot read local dictionaries", self.lookup("猫", "main.vjdict,missing.vjdict", ok=False).stdout)

    def test_glossary_limit_is_reported_and_utf8_safe(self):
        _, stats = self.build([term("猫", gloss=["説明" * 10000])])
        self.assertEqual(stats["truncated_definitions"], 1)
        result = self.lookup("猫")
        self.assertIn("[definition truncated]", result.stdout)
        self.assertNotIn("\ufffd", result.stdout)

    def test_size_limit_and_exhausted_results_are_actionable(self):
        self.build([term("猫")])
        self.assertIn("smaller OCR region", self.lookup("猫" * 1400, ok=False).stdout)
        self.assertIn("smaller OCR region", self.lookup("猫。" * 129, ok=False).stdout)

    def test_128_repeated_occurrences_keep_individual_navigation(self):
        self.build([term("猫", "ねこ", ["cat; 猫科の動物。"])])
        result = self.lookup("猫。" * 128)
        navigation = result.stdout.split("[navigation]\n", 1)[1].splitlines()
        self.assertEqual(len(navigation), 128)
        for index, line in enumerate(navigation):
            self.assertTrue(line.startswith(f"{index + 1}: "))
            self.assertEqual(line.count("【猫】"), 1)
            self.assertTrue(line.split(": ", 1)[1].startswith("猫。" * index + "【猫】。"))
        self.assertNotIn("[error]", result.stdout)

    def test_large_disk_dictionary_keeps_lookup_memory_fixed(self):
        self.build([term("猫", "ねこ")])
        small = self.lookup("猫")
        rows = [term(f"項目{i:06d}", gloss=["synthetic large-dictionary definition " * 3]) for i in range(30000)]
        rows.append(term("猫", "ねこ"))
        path, _ = self.build(rows)
        self.assertGreater(path.stat().st_size, 4 * 1024 * 1024)
        large = self.lookup("猫")
        peak = lambda result: int(re.search(r"arena peak: (\d+)", result.stderr).group(1))
        self.assertEqual(peak(small), peak(large))
        self.assertLess(peak(large), 112 * 1024)

    def test_converter_atomic_failure_preserves_previous_output(self):
        path, _ = self.build([term("猫")], format_version=1)
        original = path.read_bytes()
        bad = self.archive([term("犬"), ["malformed"]])
        with self.assertRaisesRegex(ValueError, "row 2"):
            convert.convert([bad], path)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(list(self.directory.glob("*.tmp")), [])

    def test_converter_rejects_non_term_dictionary(self):
        path = self.archive([])
        with zipfile.ZipFile(path, "w") as z:
            z.writestr("index.json", '{"format": 3, "title": "Frequency"}')
            z.writestr("term_meta_bank_1.json", '[]')
        with self.assertRaisesRegex(ValueError, "frequency/pitch/kanji-only"):
            convert.convert([path], self.directory / "main.vjdict")

    def test_streaming_json_and_invalid_separators(self):
        source = json.dumps([term("猫"), term("😀"), term("犬")], ensure_ascii=False)
        self.assertEqual(list(convert.stream_array(io.StringIO(source), 7)), json.loads(source))
        for source in ("[", "[[]", "[[],]", "[[] []]", "[] false", "{}"):
            with self.assertRaises(ValueError):
                list(convert.stream_array(io.StringIO(source), 2))

    def test_generated_table_is_reproducible(self):
        self.assertEqual(gen_deinflect.generate(), (ROOT / "core/deinflect.inc").read_text())

    def test_corrupt_index_does_not_crash_runtime(self):
        path, _ = self.build([term("猫", "ねこ")], format_version=1)
        original = path.read_bytes()
        rng = random.Random(17)
        for _ in range(24):
            bad = bytearray(original)
            offset = rng.randrange(convert.HEADER.size, convert.HEADER.size + 2 * convert.RECORD.size)
            bad[offset] ^= 0xFF
            path.write_bytes(bad)
            result = subprocess.run([str(CLI), "--dict", "local", "--local-dir", str(self.directory),
                                     "--text", "猫ネコ"], capture_output=True, timeout=10)
            self.assertIn(result.returncode, (0, 1), result.stderr)

    def test_legacy_and_hash_formats_return_identical_entries(self):
        rows = [term("猫", "ねこ", ["cat"]), term("食べる", "たべる", ["eat"], "v1"),
                term("猫", "ねこ", ["second sense"], score=0)]
        self.build(rows, "legacy.vjdict", format_version=1)
        self.build(rows, "hash.vjdict", format_version=2)
        text = "😀猫は食べませんでした。ネコ。"
        self.assertEqual(self.lookup(text, "legacy.vjdict").stdout, self.lookup(text, "hash.vjdict").stdout)
        mixed = self.lookup("猫", "legacy.vjdict,hash.vjdict").stdout
        self.assertEqual(mixed.count("second sense"), 2)
        self.assertTrue(install.inspect(self.directory / "legacy.vjdict").endswith(".vjdict"))
        self.assertTrue(install.inspect(self.directory / "hash.vjdict").endswith(".vjdict"))

    def test_hash_collision_requires_full_key_comparison(self):
        # Force a collision in the converter while retaining the real hash of
        # the query. Both groups occupy one hash range; only 猫 may match.
        with mock.patch.object(convert, "key_hash", return_value=convert.key_hash("猫".encode())):
            self.build([term("犬", gloss=["wrong dog"]), term("猫", gloss=["correct cat"])])
        result = self.lookup("猫").stdout
        self.assertIn("correct cat", result)
        self.assertNotIn("wrong dog", result)

    def test_overflow_bucket_and_invalid_cycle(self):
        # 200 keys share the real low hash bit, requiring an overflow page in
        # a two-bucket index. Find a key sorted into the overflow page.
        keys, i = [], 0
        while len(keys) < 200:
            key = f"語{i:06d}"
            if convert.key_hash(key.encode()) & 1 == 0:
                keys.append(key)
            i += 1
        path, stats = self.build([term(k) for k in keys])
        self.assertEqual(stats["hash_buckets"], 2)
        self.assertEqual(stats["index_pages"], 3)
        query = max(keys, key=lambda k: convert.key_hash(k.encode()))
        self.assertIn("definition of " + query, self.lookup(query).stdout)
        bad = bytearray(path.read_bytes())
        struct.pack_into("<Q", bad, convert.PAGE_SIZE + 8, convert.PAGE_SIZE)
        path.write_bytes(bad)
        self.assertIn("Invalid or incomplete", self.lookup(query, ok=False).stdout)

    def test_hash_format_rejects_invalid_pages_groups_and_postings(self):
        path, _ = self.build([term("猫")])
        original = path.read_bytes()
        slot = convert.PAGE_SIZE + convert.PAGE_HEADER.size
        group = struct.unpack_from("<Q", original, slot + 8)[0]
        modifications = [
            (48, "<Q", 3),  # bucket count must be a power of two
            (convert.PAGE_SIZE + 4, "<I", 171),
            (slot + 8, "<Q", 0xFFFFFFFFFFFFFFFF),
            (slot + 16, "<I", 0xFFFFFFFF),
            (slot + 20, "<H", 193),
            (group + len("猫".encode()), "<Q", len(original) + 1),
        ]
        for offset, fmt, value in modifications:
            bad = bytearray(original)
            struct.pack_into(fmt, bad, offset, value)
            path.write_bytes(bad)
            self.assertIn("Invalid or incomplete", self.lookup("猫", ok=False).stdout)
        path.write_bytes(original[:-1])
        with self.assertRaises(ValueError):
            install.inspect(path)

    def test_hash_index_reduces_reads_with_constant_memory(self):
        bench = os.environ.get("VJO_BENCH")
        if not bench:
            self.skipTest("VJO_BENCH is set by CTest")
        rows = [term(f"項目{i:06d}") for i in range(4000)] + [term("猫", "ねこ")]
        self.build(rows, "legacy.vjdict", format_version=1)
        self.build(rows, "hash.vjdict", format_version=2)
        def run(name):
            proc = subprocess.run([bench, str(self.directory), name, "猫。霻霻霻霻。猫。", "1"],
                                  capture_output=True, text=True, check=True)
            return json.loads(proc.stdout)
        before, after = run("legacy.vjdict"), run("hash.vjdict")
        self.assertEqual(before["digest"], after["digest"])
        self.assertLess(after["read_calls"], before["read_calls"] // 3)
        self.assertLess(after["arena_peak"], 112 * 1024)


class FakeFTP:
    def __init__(self, files=None, fail_replace=False):
        self.files = files or {}
        self.fail_replace = fail_replace

    def retrbinary(self, command, callback):
        name = command[5:]
        if name not in self.files:
            raise ftplib.error_perm("550 missing")
        callback(self.files[name])

    def storbinary(self, command, stream, **kwargs):
        self.files[command[5:]] = stream.read()

    def rename(self, source, dest):
        if self.fail_replace and source.endswith("config.ini.local.tmp"):
            raise ftplib.error_perm("550 rename failed")
        if dest in self.files:
            raise ftplib.error_perm("550 destination exists")
        self.files[dest] = self.files.pop(source)


class InstallerTest(unittest.TestCase):
    def test_preserve_settings_and_back_up_config(self):
        old = b"; my config\ndictionary = hachidori\njiten_api_key = secret\nanki_host = 10.0.0.3\ndictionary = jiten\n"
        key = install.DATA_DIR + "/config.ini"
        ftp = FakeFTP({key: old})
        with contextlib.redirect_stdout(io.StringIO()):
            install.enable(ftp, ["main.vjdict", "extra.vjdict"])
        config = ftp.files[key].decode()
        self.assertIn("jiten_api_key = secret", config)
        self.assertIn("anki_host = 10.0.0.3", config)
        self.assertEqual(config.count("dictionary = local"), 1)
        self.assertIn("local_dictionaries = main.vjdict,extra.vjdict", config)
        self.assertIn(old, [value for path, value in ftp.files.items() if "before-local" in path])

    def test_restore_after_failed_config_rename(self):
        key = install.DATA_DIR + "/config.ini"
        ftp = FakeFTP({key: b"dictionary = hachidori\n"}, fail_replace=True)
        with self.assertRaises(ftplib.error_perm):
            install.enable(ftp, ["main.vjdict"])
        self.assertEqual(ftp.files[key], b"dictionary = hachidori\n")

    def test_never_overwrite_an_unreadable_config(self):
        ftp = FakeFTP()
        with self.assertRaises(ftplib.error_perm):
            install.enable(ftp, ["main.vjdict"])
        self.assertEqual(ftp.files, {})


if __name__ == "__main__":
    unittest.main()
