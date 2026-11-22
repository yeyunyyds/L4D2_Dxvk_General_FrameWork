#include "fpb_projection.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

static void check(bool ok, const char *description) {
  if (!ok) throw std::runtime_error(description);
}

// Synthetic Source-like camera at (0,0,64), looking +X at pitch=0.
// Positive pitch looks down, Z is world-up.
static ldb_camera::Matrix makeVP(float pitch, bool d3dLeftHanded = false) {
  const float cp = std::cos(pitch), sp = std::sin(pitch);
  const std::array<float, 3> pos{0.f, 0.f, 64.f};
  const std::array<float, 3> axes[3]{
    {0.f, d3dLeftHanded ? -1.f : 1.f, 0.f},
    {sp, 0.f, cp}, {cp, 0.f, -sp}
  };
  ldb_camera::Matrix view{}, projection{}, vp{};
  for (int r = 0; r < 3; ++r) {
    for (int j = 0; j < 3; ++j)
      view[r * 4 + j] = axes[r][j];
    view[r * 4 + 3] = -axes[r][0] * pos[0] -
                       axes[r][1] * pos[1] -
                       axes[r][2] * pos[2];
  }
  view[15] = 1;
  constexpr float n = .5f, f = 1000.f;
  constexpr float fov = 1.2f, aspect = 1.7f;
  projection[0] = 1 / (std::tan(fov / 2) * aspect);
  projection[5] = 1 / std::tan(fov / 2);
  projection[10] = f / (f - n);
  projection[11] = -f * n / (f - n);
  projection[14] = 1;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k)
        vp[i * 4 + j] += projection[i * 4 + k] * view[k * 4 + j];
  return vp;
}

int main() {
  try {
    ldb_fpb_projection::Settings cfg{};
    auto forward = makeVP(0.f), down = makeVP(1.2f), up = makeVP(-.5f);
    check(!cfg.enabled, "projection must be opt-in");
    check(ldb_fpb_projection::project(down, 1920, 1080, cfg).count == 0,
          "disabled projection is not empty");
    cfg.enabled = true;
    auto visible = ldb_fpb_projection::project(down, 1920, 1080, cfg);
    check(visible.count > 0, "looking down should reveal proxy body");
    check(!visible.rejectedCoverage, "normal view exceeded safe coverage");
    // Side wings are the torso's oversized near-camera projection, not
    // necessarily left/right legs. Crop just the torso after projection,
    // preserving its vertical span and both leg masks byte-for-byte.
    check(visible.count == 3, "synthetic camera should show 3 FPB boxes");
    cfg.torsoScreenWidth = 0.4f;
    const auto trimmed = ldb_fpb_projection::project(
        down, 1920, 1080, cfg);
    check(trimmed.count == visible.count, "torso crop removed body boxes");
    check(trimmed.regions[0].right - trimmed.regions[0].left <
          visible.regions[0].right - visible.regions[0].left,
          "torso horizontal wings did not shrink");
    check(trimmed.regions[0].top == visible.regions[0].top &&
          trimmed.regions[0].bottom == visible.regions[0].bottom,
          "torso screen crop unexpectedly changed vertical position");
    check(std::abs(trimmed.regions[0].left + trimmed.regions[0].right -
                   visible.regions[0].left - visible.regions[0].right) <= 1,
          "torso screen crop moved the rectangle center");
    for (uint32_t i = 1; i < visible.count; ++i) {
      const auto &before = visible.regions[i], &after = trimmed.regions[i];
      check(before.left == after.left && before.right == after.right &&
            before.top == after.top && before.bottom == after.bottom,
            "torso-only crop changed a leg projection");
    }
    cfg.torsoScreenWidth = 1.0f;
    // Each rectangle has separate post-projection dimensions/offsets.
    cfg.screen[0].width = .35f;
    cfg.screen[0].height = .8f;
    cfg.screen[0].x = 50.f;
    cfg.screen[0].y = -40.f;
    auto movedTorso = ldb_fpb_projection::project(down, 1920, 1080, cfg);
    check(movedTorso.count == visible.count, "torso edit removed a box");
    check(movedTorso.regions[0].right - movedTorso.regions[0].left <
          visible.regions[0].right - visible.regions[0].left,
          "torso-only width did not shrink");
    check(movedTorso.regions[0].bottom - movedTorso.regions[0].top <=
          visible.regions[0].bottom - visible.regions[0].top,
          "torso-only height grew unexpectedly");
    check(movedTorso.regions[0].left + movedTorso.regions[0].right >
          visible.regions[0].left + visible.regions[0].right,
          "torso +X did not move to the right");
    for (uint32_t i = 1; i < visible.count; ++i) {
      const auto &a = visible.regions[i], &b = movedTorso.regions[i];
      check(a.left == b.left && a.right == b.right &&
            a.top == b.top && a.bottom == b.bottom,
            "torso-only screen adjustment changed a leg");
    }
    cfg.screen[0] = {};
    cfg.screen[1].width = .5f;
    cfg.screen[1].y = -35.f;
    auto movedLeft = ldb_fpb_projection::project(down, 1920, 1080, cfg);
    check(movedLeft.count == visible.count, "left-leg edit removed a box");
    check(movedLeft.regions[0].left == visible.regions[0].left &&
          movedLeft.regions[0].top == visible.regions[0].top,
          "left-leg edit moved torso");
    check(movedLeft.regions[1].right - movedLeft.regions[1].left <
          visible.regions[1].right - visible.regions[1].left,
          "left-leg width did not shrink");
    check(movedLeft.regions[2].left == visible.regions[2].left &&
          movedLeft.regions[2].right == visible.regions[2].right,
          "left-leg edit changed right leg");
    cfg.screen[1] = {};
    cfg.screen[2].width = 0.f;
    auto disabled = ldb_fpb_projection::project(down, 1920, 1080, cfg);
    check(disabled.count == visible.count - 1,
          "zero width should disable only the right-leg box");
    cfg.screen[2] = {};

    for (unsigned i = 0; i < visible.count; ++i) {
      const auto &r = visible.regions[i];
      check(r.valid() && r.left >= 0 && r.top >= 0 &&
            r.right <= 1920 && r.bottom <= 1080,
            "projected rectangle outside viewport");
    }
    check(ldb_fpb_projection::project(up, 1920, 1080, cfg).count == 0,
          "looking up should not protect the body");
    check(ldb_fpb_projection::project(forward, 1920, 1080, cfg).count == 0,
          "level forward camera should not see behind-camera body");
    // Regression: the real Source/D3D camera may have the opposite camera
    // handedness from our synthetic right-handed control fixture. Before
    // this fix a 90-degree rotation of camera.right reversed the proxy
    // forward direction and painted a large rectangle across screen TOP.
    auto sourceDown = ldb_fpb_projection::project(
        makeVP(1.2f, true), 1920, 1080, cfg);
    check(sourceDown.count > 0, "left-handed down camera lost body");
    check(sourceDown.regions[0].top > 1080 / 2,
          "left-handed down camera projected torso over TOP of screen");
    check(sourceDown.regions[0].bottom <= 1080,
          "left-handed torso exceeded screen boundary");
    check(ldb_fpb_projection::project(
              makeVP(-0.5f, true), 1920, 1080, cfg).count == 0,
          "left-handed up camera should have no body");
    // Regression for the former near-vertical yaw dead zone:
    // for pitch > acos(0.2) (~78.46 degrees), forward.xy length is below
    // 0.2, but the previous fallback only ran below 0.001. Consequently
    // ALL three body masks disappeared before looking straight down.
    // Crop the test masks so the separate 70% coverage safety guard cannot
    // confound this particular camera-basis regression.
    ldb_fpb_projection::Settings downward = cfg;
    downward.forward = 0.0f;
    downward.right = 0.0f;
    downward.width = 0.5f;
    for (auto &region : downward.screen) {
      region.width = .3f;
      region.height = .3f;
    }
    for (bool leftHanded : {false, true}) {
      for (float pitch : {1.35f, 1.39f, 1.45f, 1.54f,
                          1.56f, 1.569f, 1.57079632679f}) {
        const auto projected = ldb_fpb_projection::project(
            makeVP(pitch, leftHanded), 1920, 1080, downward);
        check(!projected.rejectedCoverage,
              "near-vertical continuity test unexpectedly hit area guard");
        check(projected.count > 0,
              "near-vertical camera dropped all projected body regions");
      }
    }

    auto invalid = down;
    invalid[0] = std::numeric_limits<float>::quiet_NaN();
    check(ldb_fpb_projection::project(invalid, 1920, 1080, cfg).count == 0,
          "invalid camera should fail closed");
    cfg.torsoScreenWidth = .05f;
    check(ldb_fpb_projection::project(down, 1920, 1080, cfg).count == 0,
          "too-small torso crop should fail closed");
    cfg.torsoScreenWidth = 1.1f;
    check(ldb_fpb_projection::project(down, 1920, 1080, cfg).count == 0,
          "too-large torso crop should fail closed");
    cfg.torsoScreenWidth = 1.0f;
    cfg.screen[0].width = 3.1f;
    check(ldb_fpb_projection::project(down, 1920, 1080, cfg).count == 0,
          "invalid box scale should fail closed");
    cfg.screen[0] = {};
    cfg.screen[1].x = 1921.f;
    check(ldb_fpb_projection::project(down, 1920, 1080, cfg).count == 0,
          "invalid box offset should fail closed");
    cfg.screen[1] = {};
    cfg.width = 3.0f;
    check(ldb_fpb_projection::project(down, 1920, 1080, cfg).count == 0,
          "invalid width should fail closed");
    std::cout << "FPB projection cases passed\n";
  } catch (const std::exception &e) {
    std::cerr << "FPB projection test failed: " << e.what() << '\n';
    return 1;
  }
}
