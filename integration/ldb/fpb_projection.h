#pragma once
// Independent, approximate FPB protection projection. No game entity, Studio,
// Visual plugin, DLL injection or GPU readback is needed. It estimates the
// local-player origin from the verified camera position and a tunable eye
// height. This is an opt-in heuristic, NOT a real body visibility test.
#include "fsr_camera.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>

namespace ldb_fpb_projection {
struct Settings {
  bool enabled = false;
  float forward = -13.0f; // Source world units, horizontal camera yaw only.
  float right = -1.0f;
  float up = -1.0f;
  float width = 1.0f;     // Scales torso/leg horizontal dimensions.
  float torsoScreenWidth = 1.0f; // Post-projection horizontal torso crop (0.1..1).
  float eyeHeight = 64.0f; // Estimated standing eye-to-origin distance.
};
struct Rect {
  int32_t left = 0, top = 0, right = 0, bottom = 0;
  bool valid() const { return right > left && bottom > top; }
  uint64_t area() const {
    return valid() ? uint64_t(right - left) * uint64_t(bottom - top) : 0;
  }
};
struct Projection {
  std::array<Rect, 3> regions{};
  uint32_t count = 0;
  uint64_t upperBoundPixels = 0; // Sum before union, may double count.
  bool rejectedCoverage = false;
};
// Crop only the torso rectangle *in screen space*. A near-plane torso can
// project to an enormous horizontal span even at width=0.5. Changing world
// width or moving the model also moves the center; this control leaves both
// the torso's center and its top/bottom unchanged.
inline void cropTorsoSides(Rect &r, float fraction) {
  if (!r.valid() || fraction >= 1.0f) return;
  const int32_t original = r.right - r.left;
  const int32_t target = std::max<int32_t>(
      1, int32_t(std::lround(double(original) * double(fraction))));
  r.left += (original - target) / 2;
  r.right = r.left + target;
}
struct Vec3 { double x = 0, y = 0, z = 0; };
struct Clip { double x, y, z, w; };

inline Clip clipPoint(const ldb_camera::Matrix &m, const Vec3 &p) {
  const double v[4]{p.x, p.y, p.z, 1.0};
  Clip out{};
  double *parts[4]{&out.x, &out.y, &out.z, &out.w};
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      *parts[r] += double(m[size_t(r) * 4 + c]) * v[c];
  return out;
}
inline bool nearVisible(const Clip &c) {
  return std::isfinite(c.x) && std::isfinite(c.y) &&
         std::isfinite(c.z) && std::isfinite(c.w) &&
         c.w > 1e-5 && c.z >= c.w * 0.0001 && c.z <= c.w;
}
inline bool boxRect(const ldb_camera::Matrix &vp, uint32_t width,
                    uint32_t height, Vec3 origin, Vec3 forward, Vec3 right,
                    double rightLo, double rightHi,
                    double forwardLo, double forwardHi,
                    double bottom, double top, Rect &out) {
  std::array<Clip, 8> corners{};
  for (int i = 0; i < 8; ++i) {
    const double sx = (i & 1) ? rightHi : rightLo;
    const double sy = (i & 2) ? forwardHi : forwardLo;
    const double sz = (i & 4) ? top : bottom;
    corners[i] = clipPoint(vp, {
      origin.x + right.x * sx + forward.x * sy,
      origin.y + right.y * sx + forward.y * sy,
      origin.z + sz
    });
  }
  double minX = std::numeric_limits<double>::infinity();
  double minY = minX;
  double maxX = -minX, maxY = -minX;
  auto consume = [&](const Clip &p) {
    if (!nearVisible(p)) return;
    const double x = (p.x / p.w + 1.0) * double(width) * .5;
    const double y = (1.0 - p.y / p.w) * double(height) * .5;
    if (!std::isfinite(x) || !std::isfinite(y)) return;
    minX = std::min(minX, x); maxX = std::max(maxX, x);
    minY = std::min(minY, y); maxY = std::max(maxY, y);
  };
  for (const auto &p : corners) consume(p);
  // Intersections of the box's 12 edges with the near clip plane. Merely
  // dropping behind-camera corners would truncate a partly visible torso.
  for (int a = 0; a < 8; ++a)
    for (int bit : {1, 2, 4}) {
      int b = a ^ bit;
      if (a >= b) continue;
      const auto &u = corners[a], &v = corners[b];
      const double fu = u.z - .0001 * u.w;
      const double fv = v.z - .0001 * v.w;
      if (!std::isfinite(fu) || !std::isfinite(fv) ||
          (fu < 0) == (fv < 0) || fu == fv) continue;
      const double t = fu / (fu - fv);
      if (!(t >= 0 && t <= 1)) continue;
      consume({u.x + (v.x-u.x)*t, u.y + (v.y-u.y)*t,
               u.z + (v.z-u.z)*t, u.w + (v.w-u.w)*t});
    }
  if (!std::isfinite(minX) || maxX <= 0 || maxY <= 0 ||
      minX >= width || minY >= height) return false;
  // Clip in 2D after 3D near-plane clipping. Include a small silhouette
  // margin. Reject degeneracies instead of painting a whole viewport.
  out.left = int32_t(std::max(0.0, std::floor(minX - 3.0)));
  out.top = int32_t(std::max(0.0, std::floor(minY - 3.0)));
  out.right = int32_t(std::min(double(width), std::ceil(maxX + 3.0)));
  out.bottom = int32_t(std::min(double(height), std::ceil(maxY + 3.0)));
  return out.valid();
}

inline Projection project(const ldb_camera::Matrix &vp,
                          uint32_t width, uint32_t height,
                          const Settings &s) {
  Projection result;
  if (!s.enabled || !width || !height || width > 4096 || height > 4096 ||
      !std::isfinite(s.forward) || !std::isfinite(s.right) ||
      !std::isfinite(s.up) || !std::isfinite(s.width) ||
      !std::isfinite(s.torsoScreenWidth) ||
      !std::isfinite(s.eyeHeight) || s.width < .5f || s.width > 2.5f ||
      s.torsoScreenWidth < .1f || s.torsoScreenWidth > 1.0f ||
      s.eyeHeight < 24 || s.eyeHeight > 85) return result;
  ldb_fsr::Camera cam;
  if (!ldb_fsr::cameraFromViewProjection(vp, .0254f, cam)) return result;
  // Use the camera's *actual* forward axis. Rotating camera.right by a
  // hardcoded 90 degrees assumes a specific handedness; Source/D3D-style
  // left-handed view matrices reverse that guess and can put the body in
  // front of the camera, filling the *top* of the screen on look-down.
  double rx = cam.right[0], ry = cam.right[1];
  double fx = cam.forward[0], fy = cam.forward[1];
  const double rlen = std::hypot(rx, ry);
  if (!std::isfinite(rlen) || rlen < .2) return result;
  double flen = std::hypot(fx, fy);
  // At a straight-up/down camera pitch, forward.xy vanishes; the horizontal
  // projection of camera.up still identifies yaw, with pitch determining sign.
  if (!std::isfinite(flen) || flen < .001) {
    if (!std::isfinite(cam.forward[2]) ||
        std::abs(cam.forward[2]) < .5) return result;
    const double sign = cam.forward[2] < 0.0f ? 1.0 : -1.0;
    fx = sign * cam.up[0];
    fy = sign * cam.up[1];
    flen = std::hypot(fx, fy);
  }
  if (!std::isfinite(flen) || flen < .2) return result;
  rx /= rlen; ry /= rlen;
  fx /= flen; fy /= flen;
  const Vec3 right{rx, ry, 0}, forward{fx, fy, 0};
  const Vec3 base{
    double(cam.position[0]) + s.forward * forward.x + s.right * right.x,
    double(cam.position[1]) + s.forward * forward.y + s.right * right.y,
    double(cam.position[2]) - s.eyeHeight + s.up
  };
  const double scale = s.width;
  Rect r;
  // Chest/abdomen + left and right legs: no head/weapon/hands.
  if (boxRect(vp,width,height,base,forward,right,-14*scale,14*scale,
              -9*scale,9*scale,34,62,r)) {
    cropTorsoSides(r, s.torsoScreenWidth);
    result.regions[result.count++] = r;
  }
  if (boxRect(vp,width,height,base,forward,right,-12*scale,-1*scale,
              -8*scale,8*scale,2,40,r))
    result.regions[result.count++] = r;
  if (boxRect(vp,width,height,base,forward,right,1*scale,12*scale,
              -8*scale,8*scale,2,40,r))
    result.regions[result.count++] = r;
  for (uint32_t i = 0; i < result.count; ++i)
    result.upperBoundPixels += result.regions[i].area();
  // Conservative fail-closed guard against near-camera/full-screen masks.
  if (result.upperBoundPixels > uint64_t(width) * height * 7u / 10u) {
    result.count = 0;
    result.upperBoundPixels = 0;
    result.rejectedCoverage = true;
  }
  return result;
}
} // namespace ldb_fpb_projection
