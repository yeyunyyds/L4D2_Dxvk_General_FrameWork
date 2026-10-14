#pragma once
#include "camera_state.h"
#include <algorithm>
namespace ldb_camera {
using Vector = std::array<double, 3>;

inline double dot(const Vector &a, const Vector &b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline Vector cross(const Vector &a, const Vector &b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}
inline bool normalize(Vector &a) {
  double n = std::sqrt(dot(a, a));

  if (!std::isfinite(n) || n < 1e-8)
    return false;

  for (auto &x : a)
    x /= n;
  return true;
}
struct Camera {
  Vector right{}, up{}, forward{}, position{};
  double sx = 0, sy = 0, a = 0, b = 0, handed = 0;
};

inline bool camera(const Matrix &m, Camera &c) {
  c.right = {m[0], m[1], m[2]};
  c.up = {m[4], m[5], m[6]};
  c.forward = {m[12], m[13], m[14]};

  c.sx = std::sqrt(dot(c.right, c.right));
  c.sy = std::sqrt(dot(c.up, c.up));
  double sf = std::sqrt(dot(c.forward, c.forward));

  if (std::abs(sf - 1) > 0.02 || !normalize(c.right) || !normalize(c.up) ||
      !normalize(c.forward))
    return false;

  if (std::abs(dot(c.right, c.up)) > 0.01 ||
      std::abs(dot(c.right, c.forward)) > 0.01 ||
      std::abs(dot(c.up, c.forward)) > 0.01)
    return false;

  c.handed = dot(cross(c.right, c.up), c.forward) > 0 ? 1 : -1;

  Matrix view{m[0],  m[1],  m[2],  m[3],  m[4], m[5], m[6], m[7],
              m[12], m[13], m[14], m[15], 0,    0,    0,    1};
  std::array<double, 16> inv{};

  if (!inverse(view, inv))
    return false;

  c.position = {inv[3], inv[7], inv[11]};

  c.a = (double(m[8]) * m[12] + double(m[9]) * m[13] + double(m[10]) * m[14]) /
        (sf * sf);
  c.b = double(m[11]) - c.a * m[15];

  return std::isfinite(c.a) && std::isfinite(c.b);
}
inline bool midpoint(const Matrix &previous, const Matrix &current, Matrix &mid,
                     Vector &origin, bool &perspective) {
  Camera a, b;
  perspective = camera(previous, a) && camera(current, b);

  if (!perspective) {
    if (previous[12] != 0 || previous[13] != 0 || previous[14] != 0 ||
        previous[15] != 1 || current[12] != 0 || current[13] != 0 ||
        current[14] != 0 || current[15] != 1)
      return false;

    for (size_t i = 0; i < 16; i++)
      mid[i] = (previous[i] + current[i]) * 0.5f;

    origin = {0, 0, 0};
    std::array<double, 16> inv{};
    return inverse(mid, inv);
  }
  if (a.handed != b.handed || dot(a.forward, b.forward) < 0.5 ||
      std::abs(a.sx / b.sx - 1) > 0.1 || std::abs(a.sy / b.sy - 1) > 0.1)
    return false;

  Vector right{}, up{}, forward{};
  for (size_t i = 0; i < 3; i++) {
    right[i] = a.right[i] + b.right[i];
    up[i] = a.up[i] + b.up[i];
    origin[i] = (a.position[i] + b.position[i]) * 0.5;
  }
  if (!normalize(right))
    return false;

  double projection = dot(up, right);
  for (size_t i = 0; i < 3; i++)
    up[i] -= projection * right[i];

  if (!normalize(up))
    return false;

  forward = cross(right, up);
  for (auto &x : forward)
    x *= a.handed;

  double sx = (a.sx + b.sx) * 0.5, sy = (a.sy + b.sy) * 0.5,
         da = (a.a + b.a) * 0.5, db = (a.b + b.b) * 0.5;

  for (size_t i = 0; i < 3; i++) {
    mid[i] = float(sx * right[i]);
    mid[4 + i] = float(sy * up[i]);
    mid[8 + i] = float(da * forward[i]);
    mid[12 + i] = float(forward[i]);
  }
  mid[3] = float(-sx * dot(right, origin));
  mid[7] = float(-sy * dot(up, origin));
  mid[15] = float(-dot(forward, origin));
  mid[11] = float(da * mid[15] + db);

  std::array<double, 16> inv{};
  return inverse(mid, inv);
}
template <size_t N>
inline bool frameTransforms(const Matrix &previous, const Matrix &current,
                            std::array<float, N> &rows) {
  static_assert(N == 80 || N == 96, "FG camera row ABI");
  Matrix mid{}, m{};
  Vector origin{};
  bool perspective = false;

  if (!midpoint(previous, current, mid, origin, perspective))
    return false;

  const Matrix sources[3] = {current, previous, mid};
  const Matrix destinations[3] = {mid, mid, current};

  for (size_t n = 0; n < 3; n++) {
    if (!transform(sources[n], destinations[n], m))
      return false;

    std::copy(m.begin(), m.end(), rows.begin() + n * 16);
  }
  if (perspective) {
    std::array<double, 16> inv{};

    if (!inverse(mid, inv))
      return false;

    for (size_t r = 0; r < 3; r++)
      for (size_t c = 0; c < 4; c++)
        inv[r * 4 + c] -= origin[r] * inv[12 + c];

    for (size_t c = 0; c < 4; c++)
      inv[12 + c] = 0;

    for (size_t n = 0; n < 2; n++)
      for (size_t r = 0; r < 4; r++)
        for (size_t c = 0; c < 4; c++) {
          double value = 0;
          for (size_t k = 0; k < 4; k++)
            value += sources[n][r * 4 + k] * inv[k * 4 + c];
          rows[(n + 3) * 16 + r * 4 + c] = float(value);
        }
  } else
    for (size_t n = 3; n < 5; n++)
      for (size_t i = 0; i < 16; i++)
        rows[n * 16 + i] = (i % 5 == 0) ? 1.f : 0.f;

  if constexpr (N == 96) {
    if (!transform(mid, previous, m))
      return false;
    std::copy(m.begin(), m.end(), rows.begin() + 80);
  }

  for (float x : rows)
    if (!std::isfinite(x))
      return false;

  return true;
}
} // namespace ldb_camera
