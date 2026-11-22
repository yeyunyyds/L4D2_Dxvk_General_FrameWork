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
        self.assertTrue(settings.getboolean("dynamic_models"))
        self.assertTrue(settings.getboolean("fpb_protect"))
        self.assertEqual(settings["dynamic_residual_threshold"], "auto")
        self.assertFalse(settings.getboolean("mask_debug_white"))
        self.assertFalse(settings.getboolean("content_probe"))
        self.assertFalse(settings.getboolean("monitor"))
        self.assertTrue(settings.getboolean("merge_submissions"))
        self.assertFalse(settings.getboolean("auto_capture"))

    def test_host_bootstraps_layer_results_and_monitor(self):
        source = (ROOT / "integration/ldb/fg_bootstrap.h").read_text()
        hooks = (ROOT / "integration/ldb/host-hooks.patch").read_text()
        self.assertIn('L"fg-config.ini"', source)
        self.assertIn('L"VK_ADD_LAYER_PATH"', source)
        self.assertIn('L"VK_INSTANCE_LAYERS"', source)
        self.assertIn('ldbSession / L"frame-generation"', source)
        self.assertIn("launchMonitor(state.monitorPath, directory)", source)
        self.assertIn("attachLogSession(l4d2_log_session::directory())", hooks)
        self.assertIn('L"ASTERIA_FG_CONTENT_PROBE"', source)
        self.assertIn('L"ASTERIA_FG_SEPARATE_UI"', source)
        self.assertIn('L"LDB_FG_VIEWMODEL"', source)
        self.assertIn('L"LDB_FG_FPB_PROTECT"', source)
        self.assertIn('iniBool(path, L"fpb_protect"', source)
        self.assertIn('iniText(path, L"dynamic_residual_threshold", L"auto")', source)
        self.assertIn('L"LDB_FG_DYNAMIC_RESIDUAL_THRESHOLD"', source)

    def test_disabled_fg_has_a_hard_early_exit(self):
        source = (ROOT / "integration/ldb/fg_bootstrap.h").read_text()
        disabled = source.index("if (!enabled)")
        layer = source.index('L"LDB_FG_BACKEND"', disabled)
        self.assertLess(disabled, layer)
        self.assertIn("clearManagedEnvironment();", source[disabled:layer])
        self.assertIn("!state.settings.enabled", source)

    def test_host_uses_only_standard_dxvk_interop(self):
        host = (ROOT / "integration/ldb/frame_generation.h").read_text()
        package = (ROOT / "scripts/package_ldb_capture.py").read_text()
        self.assertIn("GetVulkanHandles", host)
        self.assertIn("GetSubmissionQueue", host)
        self.assertNotIn("LdbFgQueryDeviceInfoV1", host)
        self.assertNotIn("LdbFgQueryQueueTopologyV1", host)
        self.assertIn("obtain_dxvk_runtime", package)
        self.assertIn("dxvk_private_exports_required=False", package)

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

    def test_dynamic_model_protection_is_gpu_only_and_bounded(self):
        shader = (ROOT / "shaders/frame_generate.comp").read_text()
        host = (ROOT / "integration/ldb/frame_generation.h").read_text()
        self.assertIn("bool providerDynamicAt", shader)
        self.assertIn("depthResidual||firstPersonFallback", shader)
        self.assertIn("colourResidual>p.thresholds.w*2.0", shader)
        self.assertIn("dynamic_protected_pixels", host)
        self.assertIn("config().dynamicModels ? 16u : 0u", host)
        self.assertNotIn("GetRenderTargetData", host)

    def test_fpb_compat_uses_existing_gpu_overlay_without_new_hooks(self):
        host = (ROOT / "integration/ldb/frame_generation.h").read_text()
        shader = (ROOT / "shaders/frame_generate.comp").read_text()
        bootstrap = (ROOT / "integration/ldb/fg_bootstrap.h").read_text()
        self.assertIn('fpbProtect = readEnv(L"LDB_FG_FPB_PROTECT") == L"1"', host)
        self.assertIn('dynamicResidualThreshold = dynamicModels && fpbProtect ? 0.035f : 0.08f', host)
        self.assertIn('providerParams.thresholds[3] = config().dynamicResidualThreshold;', host)
        self.assertIn('L"LDB_FG_FPB_PROTECT", flag(settings.fpbProtect)', bootstrap)
        self.assertIn('float alpha=hud||viewmodel||dynamicModel?1.0:0.0;', shader)
        self.assertIn('mark=vec4(fallback.rgb,alpha);', shader)
        self.assertNotIn('GetRenderTargetData', host)

    def test_dynamic_residual_ini_numeric_range_is_validated(self):
        bootstrap = (ROOT / "integration/ldb/fg_bootstrap.h").read_text()
        host = (ROOT / "integration/ldb/frame_generation.h").read_text()
        self.assertIn('setEnvironment(L"LDB_FG_DYNAMIC_RESIDUAL_THRESHOLD"', bootstrap)
        self.assertIn('narrowAscii(state.settings.dynamicResidualThreshold)', bootstrap)
        self.assertIn('static_cast<char>(ch)', bootstrap)
        self.assertNotIn('std::string(state.settings.dynamicResidualThreshold.begin()', bootstrap)
        self.assertIn('readEnv(L"LDB_FG_DYNAMIC_RESIDUAL_THRESHOLD")', host)
        self.assertIn('std::stof(residualOverride, &used)', host)
        self.assertIn('value < 0.001f || value > 0.5f', host)
        self.assertIn('dynamicResidualOverrideRejected = true;', host)
        self.assertIn('dynamic_residual_threshold', host)

    def test_white_debug_visualizes_actual_post_fsr_protection(self):
        ini = (ROOT / "integration/ldb/fg-config.ini").read_text()
        bootstrap = (ROOT / "integration/ldb/fg_bootstrap.h").read_text()
        patch = (ROOT / "integration/fg/async-hud-x64.patch").read_text()
        shader = (ROOT / "shaders/hud_composite.comp").read_text()
        white = (ROOT / "integration/fg/HudCompositeWhiteSpirv.h").read_text()
        prepare = (ROOT / "scripts/prepare_asteria_layer.py").read_text()
        self.assertIn("mask_debug_white=0", ini)
        self.assertIn('iniBool(path, L"mask_debug_white", result.maskDebugWhite)', bootstrap)
        self.assertIn('L"LDB_FG_MASK_DEBUG_WHITE", flag(settings.maskDebugWhite)', bootstrap)
        self.assertIn('L"LDB_FG_MASK_DEBUG_WHITE", maskDebugValue, 8', patch)
        self.assertIn("maskDebugWhite ? HudCompositeWhiteSpirv.data()", patch)
        self.assertIn("HudCompositeWhiteSpirv.h", prepare)
        self.assertIn("HudCompositeSpirv[494] == 0x00040063u", white)
        self.assertIn("words[494 + 7 + 3] = 0x0000007au", white)
        self.assertIn("if (covered)", shader)
        self.assertIn("for (int y = -1; y <= 1", shader)
        self.assertIn("if (separateUi && !RecordHudComposite(state, cmd", patch)

    def test_hud_shader_is_checked_in_as_valid_spirv_words(self):
        header = (ROOT / "integration/fg/HudCompositeSpirv.h").read_text()
        shader = (ROOT / "shaders/hud_composite.comp").read_text()
        self.assertIn("0x07230203u", header)
        self.assertIn("HudCompositeSpirvSize", header)
        self.assertIn("writeonly uniform image2D generatedOutput", shader)

    def test_package_is_extract_and_run_layout(self):
        package = (ROOT / "scripts/package_ldb_capture.py").read_text()
        prefix = "bin/.l4d2bridge/"
        self.assertIn(prefix + "fg-config.ini", package)
        self.assertIn(prefix + "fg_live_hud.ps1", package)
        self.assertIn(prefix + "vulkan-layer/{filename}", package)
        self.assertIn(prefix + "capture-build.json", package)
        self.assertIn("docs/package-readme.md", package)
        self.assertIn("Every package payload must stay below bin/", package)
        for obsolete in (
                "trigger_capture.cmd", "start_fg_test.cmd",
                "manage_fg_test.ps1", "sources/hud_composite.comp",
                "docs/previous-prototype.md"):
            self.assertNotIn(obsolete, package)


if __name__ == "__main__":
    unittest.main()
