#pragma once
#include "camera_state.h"
#include <array>
#include <cmath>

namespace ldb_fsr {
struct Camera {
  float nearPlane = 0, farPlane = 0, verticalFov = 0;
  float metersPerUnit = 0;
  std::array<float, 3> position{}, up{}, right{}, forward{};
};

// Existing Host shader observations provide a row-major world->clip cViewProj.
// Recover a finite, unjittered D3D [0,1] perspective camera without changing
// game threading or forcing fixed-function rendering. Unknown conventions fail
// closed.
inline bool cameraFromViewProjection(const ldb_camera::Matrix &vp,
                                     float metersPerUnit, Camera &output) {
  if (!std::isfinite(metersPerUnit) || metersPerUnit <= 0)
    return false;
  std::array<double, 16> inverse{};
  if (!ldb_camera::inverse(vp, inverse))
    return false;
  using Vec = std::array<double, 3>;
  auto point = [&](std::array<double, 4> clip, Vec &world) {
    std::array<double, 4> h{};
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        h[r] += inverse[r * 4 + c] * clip[c];
    if (!std::isfinite(h[3]) || std::abs(h[3]) < 1e-12)
      return false;
    for (int i = 0; i < 3; ++i) {
      world[i] = h[i] / h[3];
      if (!std::isfinite(world[i]))
        return false;
    }
    return true;
  };
  auto subtract = [](Vec a, Vec b) {
    for (int i = 0; i < 3; ++i)
      a[i] -= b[i];
    return a;
  };
  auto length = [](Vec a) {
    return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  };
  auto dot = [](Vec a, Vec b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  };
  auto normalize = [&](Vec &a) {
    double n = length(a);
    if (!std::isfinite(n) || n < 1e-9)
      return false;
    for (double &v : a)
      v /= n;
    return true;
  };
  Vec origin{}, near{}, far{}, x{}, y{};
  if (!point({0, 0, 1, 0}, origin) || !point({0, 0, 0, 1}, near) ||
      !point({0, 0, 1, 1}, far) || !point({1, 0, 0, 1}, x) ||
      !point({0, 1, 0, 1}, y))
    return false;
  Vec forward = subtract(near, origin), farDirection = subtract(far, origin);
  Vec right = subtract(x, near), up = subtract(y, near);
  double n = length(forward), f = length(farDirection), halfHeight = length(up);
  if (!(n > 0 && f > n) || !normalize(forward) || !normalize(farDirection) ||
      !normalize(right) || !normalize(up) ||
      dot(forward, farDirection) < 0.99999 ||
      std::abs(dot(forward, right)) > 1e-4 ||
      std::abs(dot(forward, up)) > 1e-4 || std::abs(dot(right, up)) > 1e-4)
    return false;
  Camera camera;
  camera.nearPlane = float(n);
  camera.farPlane = float(f);
  camera.verticalFov = float(2 * std::atan(halfHeight / n));
  camera.metersPerUnit = metersPerUnit;
  if (!(camera.nearPlane > 0) || !(camera.farPlane > camera.nearPlane) ||
      !std::isfinite(camera.farPlane) ||
      !(camera.verticalFov > 0 && camera.verticalFov < 3.14))
    return false;
  for (int i = 0; i < 3; ++i) {
    camera.position[i] = float(origin[i]);
    if (!std::isfinite(camera.position[i]))
      return false;
    camera.forward[i] = float(forward[i]);
    camera.right[i] = float(right[i]);
    camera.up[i] = float(up[i]);
  }
  output = camera;
  return true;
}
} // namespace ldb_fsr
