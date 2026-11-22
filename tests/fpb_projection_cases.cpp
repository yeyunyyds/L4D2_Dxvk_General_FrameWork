#include "fpb_projection.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

static void check(bool ok, const char *description) {
  if (!ok) throw std::runtime_error(description);
}

// Synthetic Source-like camera at (0,0,64), looking +X at pitch=0.
// Positive pitch looks down, Z is world-up.
static ldb_camera::Matrix makeVP(float pitch) {
  const float cp = std::cos(pitch), sp = std::sin(pitch);
  const std::array<float, 3> pos{0.f, 0.f, 64.f};
  const std::array<float, 3> axes[3]{
    {0.f, 1.f, 0.f}, {sp, 0.f, cp}, {cp, 0.f, -sp}
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
    auto forward = makeVP(0.f), down = makeVP(1.0f), up = makeVP(-.5f);
    check(!cfg.enabled, "projection must be opt-in");
    check(ldb_fpb_projection::project(down, 1920, 1080, cfg).count == 0,
          "disabled projection is not empty");
    cfg.enabled = true;
    auto visible = ldb_fpb_projection::project(down, 1920, 1080, cfg);
    check(visible.count > 0, "looking down should reveal proxy body");
    check(!visible.rejectedCoverage, "normal view exceeded safe coverage");
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
    auto invalid = down;
    invalid[0] = std::numeric_limits<float>::quiet_NaN();
    check(ldb_fpb_projection::project(invalid, 1920, 1080, cfg).count == 0,
          "invalid camera should fail closed");
    cfg.width = 3.0f;
    check(ldb_fpb_projection::project(down, 1920, 1080, cfg).count == 0,
          "invalid width should fail closed");
    std::cout << "FPB projection cases passed\n";
  } catch (const std::exception &e) {
    std::cerr << "FPB projection test failed: " << e.what() << '\n';
    return 1;
  }
}
