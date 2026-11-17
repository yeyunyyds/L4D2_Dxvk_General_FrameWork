import configparser
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class FrameGenerationDropInTests(unittest.TestCase):
    def test_safe_drop_in_defaults(self):
        parser = configparser.ConfigParser()
        parser.read(ROOT / "integration/ldb/fg-config.ini", encoding="utf-8")
        settings = parser["frame_generation"]
        self.assertTrue(settings.getboolean("enabled"))
        self.assertTrue(settings.getboolean("hud"))
        self.assertFalse(settings.getboolean("content_probe"))
        self.assertTrue(settings.getboolean("monitor"))
        self.assertTrue(settings.getboolean("merge_submissions"))

    def test_host_bootstraps_layer_results_and_monitor(self):
        source = (ROOT / "integration/ldb/fg_bootstrap.h").read_text()
        self.assertIn('L"fg-config.ini"', source)
        self.assertIn('L"VK_ADD_LAYER_PATH"', source)
        self.assertIn('L"VK_INSTANCE_LAYERS"', source)
        self.assertIn('L"fg-results"', source)
        self.assertIn("launchMonitor(monitorPath, session)", source)
        self.assertIn('L"ASTERIA_FG_CONTENT_PROBE"', source)
        self.assertIn('L"ASTERIA_FG_SEPARATE_UI"', source)

    def test_hud_lease_waits_for_interpolation_and_present(self):
        patch = (ROOT / "integration/fg/asteria-x64.patch").read_text()
        self.assertIn("presentCompletionValue", patch)
        self.assertIn("interpolationCompleted && presentationCompleted", patch)
        self.assertIn("TimedWaitForPresents(state) == FFX_OK", patch)
        self.assertIn("scheduledOutputPresents += callbackRecorded ? 2 : 1", patch)

    def test_package_keeps_drop_in_files_below_bin(self):
        package = (ROOT / "scripts/package_ldb_capture.py").read_text()
        prefix = "bin/.l4d2bridge/"
        self.assertIn(prefix + "fg-config.ini", package)
        self.assertIn(prefix + "fg_live_hud.ps1", package)
        self.assertIn(prefix + "vulkan-layer/{filename}", package)


if __name__ == "__main__":
    unittest.main()
