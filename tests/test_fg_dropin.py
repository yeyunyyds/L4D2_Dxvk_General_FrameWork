import configparser
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class FrameGenerationDropInTests(unittest.TestCase):
    def test_bridge_baseline_is_ldb_134(self):
        baseline = json.loads(
            (ROOT / "integration/ldb/baseline.json").read_text())
        hooks = (ROOT / "integration/ldb/host-hooks.patch").read_text()
        self.assertEqual(baseline["bridge_version"], "v1.3.4")
        self.assertEqual(
            baseline["bridge_commit"],
            "78f8c17aa8eb6c752a6d790e166c9ba55ed4a91e")
        self.assertIn("lookupHostHandle", hooks)
        self.assertIn("if (pD3DDevice != nullptr)", hooks)
        self.assertEqual(hooks.count("ldb_fg::beforeDraw"), 4)
        self.assertEqual(hooks.count("ldb_fg::present"), 2)

    def test_safe_drop_in_defaults(self):
        parser = configparser.ConfigParser()
        parser.read(ROOT / "integration/ldb/fg-config.ini", encoding="utf-8")
        settings = parser["frame_generation"]
        self.assertTrue(settings.getboolean("enabled"))
        self.assertTrue(settings.getboolean("hud"))
        self.assertTrue(settings.getboolean("viewmodel"))
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
        self.assertIn('L"LDB_FG_VIEWMODEL"', source)

    def test_hud_composes_on_async_callback_and_retires_on_its_timeline(self):
        patch = (ROOT / "integration/fg/async-hud-x64.patch").read_text()
        host = (ROOT / "integration/ldb/frame_generation.h").read_text()
        hooks = (ROOT / "integration/ldb/host-hooks.patch").read_text()
        self.assertIn("RecordHudComposite(state, cmd", patch)
        self.assertIn("+    fgInfo.compositionMode = VK_COMPOSITION_MODE_PRESENT_QUEUE_FFX;", patch)
        self.assertIn("+        if (interpolationCompleted)", patch)
        self.assertIn("snapshotBeforeHud", host)
        self.assertIn("viewmodelBeforeSnapshot", host)
        self.assertIn("viewmodelAfterSnapshot", host)
        self.assertIn("withTransferredColorSources<4>", host)
        self.assertIn("device->StretchRect", host)
        self.assertNotIn("transfer(back.get(), ResidentFg::CurrentUi)", host)
        self.assertEqual(hooks.count("+        ldb_fg::beforeReset(pD3DDevice);"), 2)

    def test_viewmodel_uses_exact_current_overlay_mask(self):
        shader = (ROOT / "shaders/frame_generate.comp").read_text()
        host = (ROOT / "integration/ldb/frame_generation.h").read_text()
        self.assertIn("sampleAt(currentNearBefore,screen).rgb", shader)
        self.assertIn("sampleAt(currentNearAfter,screen).rgb", shader)
        self.assertIn("sampleAt(previousNearBefore,screen).rgb", shader)
        self.assertIn("currentViewmodel||previousViewmodel", shader)
        self.assertIn("hud||viewmodel", shader)
        self.assertIn("viewmodel_overlay_available", host)

    def test_hud_shader_is_checked_in_as_valid_spirv_words(self):
        header = (ROOT / "integration/fg/HudCompositeSpirv.h").read_text()
        shader = (ROOT / "shaders/hud_composite.comp").read_text()
        self.assertIn("0x07230203u", header)
        self.assertIn("HudCompositeSpirvSize", header)
        self.assertIn("writeonly uniform image2D generatedOutput", shader)

    def test_package_keeps_drop_in_files_below_bin(self):
        package = (ROOT / "scripts/package_ldb_capture.py").read_text()
        prefix = "bin/.l4d2bridge/"
        self.assertIn(prefix + "fg-config.ini", package)
        self.assertIn(prefix + "fg_live_hud.ps1", package)
        self.assertIn(prefix + "vulkan-layer/{filename}", package)
        self.assertIn("sources/hud_composite.comp", package)


if __name__ == "__main__":
    unittest.main()
