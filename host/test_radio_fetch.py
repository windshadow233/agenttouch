"""Native regression checks for the firmware's RSS transport retry policy."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class RssFetchTests(unittest.TestCase):
    def test_native_rss_fetch(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler is not installed")
        root = pathlib.Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as tmp:
            binary = str(pathlib.Path(tmp) / "rss-fetch-test")
            subprocess.run([
                compiler, "-std=c++11", "-I", str(root / "firmware/src"),
                str(root / "firmware/tests/rss_fetch_test.cpp"), "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
