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
        self.assertEqual(settings["first_person_fallback_start"], "0.28")
        self.assertEqual(settings.getint("dynamic_mask_dilation"), 1)
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
        self.assertIn('L"LDB_FG_FIRST_PERSON_FALLBACK_START"', source)
        self.assertIn('L"LDB_FG_DYNAMIC_MASK_DILATION"', source)

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
        self.assertIn('providerParams.thresholds[3] = liveProtection.residual;', host)
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

    def test_fallback_and_dilation_are_validated_and_reach_gpu_pipeline(self):
        bootstrap = (ROOT / "integration/ldb/fg_bootstrap.h").read_text()
        host = (ROOT / "integration/ldb/frame_generation.h").read_text()
        gpu = (ROOT / "tools/resident_frame_generator.h").read_text()
        layer = (ROOT / "integration/fg/async-hud-x64.patch").read_text()
        self.assertIn('iniText(path, L"first_person_fallback_start", L"0.28")', bootstrap)
        self.assertIn('iniUint(path, L"dynamic_mask_dilation", 1, 1)', bootstrap)
        self.assertIn('readEnv(L"LDB_FG_FIRST_PERSON_FALLBACK_START")', host)
        self.assertIn('readEnv(L"LDB_FG_DYNAMIC_MASK_DILATION")', host)
        self.assertIn('value < 0.0f || value > 1.0f', host)
        self.assertIn('value > 1u', host)
        self.assertIn('config().firstPersonFallbackStart', host)
        self.assertIn('config().dynamicMaskDilation', host)
        self.assertIn('fgShader[758] = encoded;', gpu)
        self.assertIn('fgShader[31858u + 12u + 2u] = zeroArray;', gpu)
        self.assertIn('fgShader[31860] != 494u', gpu)
        self.assertNotIn('fgShader[706u + i] = zeroVector;', gpu)
        self.assertIn('L"LDB_FG_DYNAMIC_MASK_DILATION", dilationValue, 8', layer)
        self.assertIn('compositeWords[304 + shift] = 0x41u;', layer)
        self.assertIn('compositeWords[347 + shift] = 0x41u;', layer)

    def test_pinned_shader_patching_targets_exact_spirv_instructions(self):
        import re
        fg = (ROOT / "tools/frame_generate_spirv.h").read_text()
        hud = (ROOT / "integration/fg/HudCompositeSpirv.h").read_text()
        fw = [int(s, 16) for s in re.findall(r'0x([0-9a-fA-F]+)', fg)]
        hw = [int(s, 16) for s in re.findall(r'0x([0-9a-fA-F]+)u', hud)]
        self.assertEqual(fw[755:759], [0x0004002b, 8, 765, 0x3e8f5c29])
        self.assertEqual(fw[698:705], [0x0007002c, 488, 494, 489, 491, 492, 493])
        self.assertEqual(fw[3], 0xaea0)
        self.assertEqual(hw[301], 0x000700f5)
        self.assertEqual(hw[308], 0x000500b3)
        self.assertEqual(hw[336], 0x000700f5)
        self.assertEqual(hw[343], 0x000500b3)
        self.assertEqual([hw[i] for i in (304, 339)], [0x27, 0x27])
        self.assertEqual([hw[i] for i in (312, 347)], [0x2e, 0x2e])

    def test_all_four_inis_hot_reload_without_game_restart(self):
        host = (ROOT / "integration/ldb/frame_generation.h").read_text()
        gpu = (ROOT / "tools/resident_frame_generator.h").read_text()
        layer = (ROOT / "integration/fg/async-hud-x64.patch").read_text()
        self.assertIn('liveIniPath', host)
        self.assertIn('std::filesystem::last_write_time(liveIniPath, ec)', host)
        self.assertIn('tick - liveIniPollTick < 500', host)
        self.assertIn('refreshLiveProtection(uid);', host)
        self.assertIn('L"dynamic_residual_threshold"', host)
        self.assertIn('L"first_person_fallback_start"', host)
        self.assertIn('L"dynamic_mask_dilation"', host)
        self.assertIn('L"mask_debug_white"', host)
        self.assertIn('gpu->updateProtectionShader(updated.fallbackStart,', host)
        self.assertIn('providerParams.thresholds[3] = liveProtection.residual;', host)
        self.assertIn('L"LDB_FG_RUNTIME_COMPOSITE_MODE",', host)
        self.assertIn('liveIniRejectedStamp', host)
        self.assertIn('verifiedStamp != stamp', host)
        self.assertIn('liveIniStamp = stamp;', host)
        self.assertIn('live_protection_updated', host)
        self.assertIn('live_protection_rejected', host)
        self.assertIn('createTunedGenerateShader(float fallbackStart,', gpu)
        self.assertIn('updateProtectionShader(float fallbackStart,', gpu)
        self.assertIn('c.vkQueueWaitIdle(c.queue)', gpu)
        self.assertIn('VkPipeline previous = pipeline;', gpu)
        self.assertIn('std::array<VkPipeline, 4> hudPipelines{};', layer)
        self.assertIn('for (uint32_t mode = 0; mode < 4; ++mode)', layer)
        self.assertIn('state.hudPipelines[mode]', layer)
        self.assertIn('const VkPipeline activePipeline = state.hudPipelines[mode]', layer)
        self.assertIn('L"LDB_FG_RUNTIME_COMPOSITE_MODE", modeValue, 8', layer)
        self.assertIn('L"LDB_FG_MASK_DEBUG_WHITE", whiteValue, 8', layer)
        self.assertIn('L"LDB_FG_DYNAMIC_MASK_DILATION", dilationValue, 8', layer)
        self.assertIn('VK_PIPELINE_BIND_POINT_COMPUTE, activePipeline', layer)

    def test_no_dilation_preserves_other_shared_offset_users(self):
        import re
        gpu = (ROOT / "tools/resident_frame_generator.h").read_text()
        shader = (ROOT / "tools/frame_generate_spirv.h").read_text()
        words = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]+)', shader)]
        self.assertEqual(words[31858:31861], [0x0003003e, 3049, 494])
        self.assertEqual([words[i] for i in (2511, 8263, 8322)], [494, 494, 494])
        # Exact pinned transformation must replace only the providerDynamic
        # store, not the shared constant used by foregroundEdge/temporalChange.
        zero_vector = words[3]
        zero_array = zero_vector + 1
        inserted = [0x0005002c, 16, zero_vector, 378, 378,
                    0x0007002c, 488, zero_array,
                    zero_vector, zero_vector, zero_vector, zero_vector]
        patched = words[:698] + inserted + words[698:]
        patched[3] = zero_array + 1
        patched[31858 + 12 + 2] = zero_array
        self.assertEqual(patched[31860 + 12], zero_array)
        self.assertEqual([patched[i + 12] for i in (2511, 8263, 8322)],
                         [494, 494, 494])
        self.assertEqual(patched[698 + 12:705 + 12], words[698:705])
        self.assertIn('fgShader[31858u + 12u + 2u] = zeroArray;', gpu)
        i = 5
        while i < len(patched):
            instruction_words = patched[i] >> 16
            self.assertGreater(instruction_words, 0)
            i += instruction_words
        self.assertEqual(i, len(patched))

    def test_fpb_projection_is_opt_in_and_every_setting_is_hot(self):
        import re
        import math
        config = configparser.ConfigParser()
        config.read(ROOT / "integration/ldb/fg-config.ini", encoding="utf-8")
        ini = config["frame_generation"]
        self.assertFalse(ini.getboolean("fpb_projection_protect"))
        self.assertEqual(ini.getfloat("fpb_projection_forward"), -13.0)
        self.assertEqual(ini.getfloat("fpb_projection_right"), -1.0)
        self.assertEqual(ini.getfloat("fpb_projection_up"), -1.0)
        self.assertEqual(ini.getfloat("fpb_projection_width"), 1.0)
        self.assertEqual(ini.getfloat("fpb_projection_eye_height"), 64.0)

        host = (ROOT / "integration/ldb/frame_generation.h").read_text()
        model = (ROOT / "integration/ldb/fpb_projection.h").read_text()
        gpu = (ROOT / "tools/resident_frame_generator.h").read_text()
        prepare = (ROOT / "scripts/prepare_ldb_capture.py").read_text()
        for key in ("protect", "forward", "right", "up",
                    "width", "eye_height"):
            self.assertIn(f'L"fpb_projection_{key}"', host)
        self.assertIn('readValue(L"fpb_projection_protect", L"0")', host)
        self.assertIn("refreshLiveProtection(uid);", host)
        self.assertIn("updated.projectedBody.enabled =", host)
        self.assertIn("updated.projectedBody.forward = parseFloat(", host)
        self.assertIn("updated.projectedBody.right = parseFloat(", host)
        self.assertIn("updated.projectedBody.up = parseFloat(", host)
        self.assertIn("updated.projectedBody.width = parseFloat(", host)
        self.assertIn("updated.projectedBody.eyeHeight = parseFloat(", host)
        self.assertIn("liveProtection = updated;", host)
        self.assertIn("const auto projectedBody = ldb_fpb_projection::project(", host)
        self.assertIn("if (projectedBody.count && gpu->canBlitProjectedBody())", host)
        self.assertIn("sceneOnly = false;", host)
        self.assertIn("&projectedBody);", host)
        self.assertIn('emit("fpb_projection"', host)

        # True 3D region projection, not a screen-height fallback or fixed
        # rectangle; no connection to Visual/Studio/32-bit model hooks.
        self.assertIn("cameraFromViewProjection", model)
        self.assertIn("cam.position[2]) - s.eyeHeight + s.up", model)
        self.assertIn("for (int bit : {1, 2, 4})", model)
        self.assertIn("if (!s.enabled", model)
        self.assertIn("if (result.upperBoundPixels >", model)
        self.assertIn("result.rejectedCoverage = true;", model)
        self.assertNotIn("L4D2Visual", gpu)
        self.assertNotIn("StudioModel", model)
        self.assertIn("'integration/ldb/fpb_projection.h'", prepare)
        self.assertIn("'fpb_projection.h'", prepare)

        # The sampled source view forces A=1; the already-vetted compute
        # compositor explicitly writes RGBA(alpha=1) to the ROI. Never use
        # a blit that accidentally copies undefined D3D9 backbuffer alpha.
        self.assertIn('#include "HudCompositeSpirv.h"', gpu)
        self.assertIn('VK_COMPONENT_SWIZZLE_ONE', gpu)
        self.assertIn('VK_PIPELINE_CREATE_DISPATCH_BASE_BIT', gpu)
        self.assertIn('VkDescriptorSet projectionDescriptorSet', gpu)
        self.assertIn('projectionDescriptorSet != VK_NULL_HANDLE;', gpu)
        self.assertIn('if (canOverlayProjectedBody()) return true;', gpu)
        self.assertIn('const ldb_fpb_projection::Projection *bodyProjection', gpu)
        self.assertIn('auto &src = images[CurrentFinal];', gpu)
        self.assertIn('auto &dst = images[providerUi(activeProviderSlot)];', gpu)
        self.assertIn('vkCmdDispatchBase(c.command, x, y, 0', gpu)
        self.assertIn('projectionPipeline = VK_NULL_HANDLE;', gpu)
        self.assertIn('gpu->ensureProjectionPipeline();', host)
        self.assertIn('gpu->canOverlayProjectedBody()', host)
        self.assertIn('gpu_overlay_available', host)
        self.assertIn("'HudCompositeSpirv.h'", prepare)
        self.assertNotIn('vkCmdBlitImage', gpu)
        self.assertNotIn('GetRenderTargetData', host)
        self.assertIn('vec4(exactCurrentPixel.rgb, 1.0)', (
            ROOT / 'shaders/hud_composite.comp').read_text())

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
        self.assertIn('L"LDB_FG_MASK_DEBUG_WHITE", whiteValue, 8', patch)
        self.assertIn("HudCompositeWhiteSpirv.begin(), HudCompositeWhiteSpirv.end()", patch)
        self.assertIn("shaderInfo.pCode = compositeWords.data();", patch)
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
