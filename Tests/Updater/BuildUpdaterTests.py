"""Reject unsafe build-time pin configurations before invoking the Swift compiler."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("build_updater", Path(__file__).resolve().parents[2] / "scripts/build-updater.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class BuildUpdaterTests(unittest.TestCase):
    def test_valid_current_and_rotation_pairs(self):
        self.assertEqual(builder.validate_pins(["A" * 64, "B" * 64, "C" * 64, "D" * 64]),
                         ["a" * 64, "b" * 64, "c" * 64, "d" * 64])
        self.assertEqual(builder.validate_pins(["", "", "", ""]), ["", "", "", ""])

    def test_invalid_pin_configurations(self):
        for values in [["a" * 64, "", "", ""], ["", "", "c" * 64, "d" * 64],
                       ["a" * 64, "b" * 64, "c" * 64, ""],
                       ["a" * 64, "b" * 64, "a" * 64, "b" * 64],
                       ["a" * 63, "b" * 64, "", ""], ["g" * 64, "b" * 64, "", ""],
                       ["a" * 64 + '\"; fatalError()', "b" * 64, "", ""]]:
            with self.subTest(values=values), self.assertRaises(ValueError):
                builder.validate_pins(values)


if __name__ == "__main__":
    unittest.main()
