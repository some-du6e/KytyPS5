"""Golden tests: each fixture in tests/fixtures has tests/expected/<case>.json with the key fields."""

import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import triage_test_util as u  # noqa: E402

MAX_FIXTURE_BYTES = 200 * 1024
MAX_TOTAL_BYTES = 2 * 1024 * 1024


class GoldenTests(unittest.TestCase):
    def test_every_fixture_has_a_golden(self):
        cases = sorted(n[:-4] for n in os.listdir(u.FIXTURES) if n.endswith(".txt"))
        goldens = sorted(n[:-5] for n in os.listdir(u.EXPECTED) if n.endswith(".json"))
        self.assertEqual(cases, goldens)

    def test_goldens(self):
        for name in sorted(os.listdir(u.EXPECTED)):
            if not name.endswith(".json"):
                continue
            case = name[:-5]
            with self.subTest(case=case):
                expected = u.load_json(os.path.join(u.EXPECTED, name))
                got = u.golden_summary(u.report(case + ".txt"))
                for key, want in expected.items():
                    if key.startswith("_"):
                        continue
                    if key.endswith("_includes"):
                        field = key[:-len("_includes")]
                        for item in want:
                            self.assertIn(item, got[field], "%s: %s" % (case, field))
                        continue
                    self.assertEqual(got.get(key), want, "%s: %s" % (case, key))


class FixtureHygieneTests(unittest.TestCase):
    def setUp(self):
        self.index = {e["case"]: e for e in u.load_json(os.path.join(u.FIXTURES, "INDEX.json"))}

    def test_index_lists_every_fixture_with_provenance(self):
        files = sorted(n for n in os.listdir(u.FIXTURES) if n.endswith(".txt"))
        self.assertEqual(sorted(e["file"] for e in self.index.values()), files)
        for case, e in self.index.items():
            with self.subTest(case=case):
                if e["synthetic"]:
                    self.assertTrue(case.startswith("synthetic-"))
                    self.assertTrue(e.get("description"))
                    with open(u.fixture(e["file"]), "r", encoding="utf-8") as f:
                        self.assertIn("SYNTHETIC", f.readline())
                else:
                    self.assertRegex(e["issue"], r"^https://github\.com/KytyPS5/KytyPS5/issues/\d+$")
                    self.assertRegex(e["sha256"], r"^[0-9a-f]{64}$")
                    self.assertTrue(e["original_filename"])
                    self.assertTrue(e["redacted"])

    def test_sizes(self):
        total = 0
        for n in os.listdir(u.FIXTURES):
            size = os.path.getsize(u.fixture(n))
            total += size
            self.assertLess(size, MAX_FIXTURE_BYTES, n)
        self.assertLess(total, MAX_TOTAL_BYTES)

    def test_no_user_names_or_game_folders(self):
        bad = [re.compile(rb"(?i)[A-Za-z]:[\\/]+Users[\\/]+(?!<redacted>)[^\\/\s]"),
               re.compile(rb"/home/(?!runner/|<redacted>)[^/\s]"),
               re.compile(rb"(?<![A-Za-z]:)(?<![\\/A-Za-z])/Users/(?!runner/|Shared/|<redacted>)[^/\s]"),
               re.compile(rb"(?i)\[SuperPSX\]|\[DLPSGAME")]
        for n in sorted(os.listdir(u.FIXTURES)):
            with open(u.fixture(n), "rb") as f:
                data = f.read()
            for pat in bad:
                m = pat.search(data)
                self.assertIsNone(m, "%s: %r" % (n, m.group(0) if m else None))


if __name__ == "__main__":
    unittest.main()
