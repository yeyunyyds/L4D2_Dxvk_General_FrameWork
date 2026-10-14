#include "frame_math.h"
#include <iostream>
#include <stdexcept>
using namespace ldb_camera;
void check(bool value, const char *text) {
  if (!value)
    throw std::runtime_error(text);
}
Matrix perspective(double yaw, Vector eye) {
  double c = std::cos(yaw), s = std::sin(yaw);
  Vector r{c, 0, -s}, u{0, 1, 0}, f{s, 0, c};
  double a = 1.001, b = -.1;
  return {float(r[0]),
          0,
          float(r[2]),
          float(-dot(r, eye)),
          0,
          1,
          0,
          float(-dot(u, eye)),
          float(a * f[0]),
          0,
          float(a * f[2]),
          float(b - a * dot(f, eye)),
          float(f[0]),
          0,
          float(f[2]),
          float(-dot(f, eye))};
}
int main() {
  try {
    Matrix a = perspective(0, {0, 0, 0}), b = perspective(0, {2, 0, 0}), m{};
    Vector origin{};
    bool perspectiveMode = false;
    check(midpoint(a, b, m, origin, perspectiveMode) && perspectiveMode,
          "perspective midpoint unavailable");
    check(std::abs(origin[0] - 1) < 1e-6, "translation midpoint incorrect");
    b = perspective(.3490658503988659, {0, 0, 0});
    check(midpoint(a, b, m, origin, perspectiveMode),
          "rotation midpoint unavailable");
    check(std::abs(m[12] - std::sin(.17453292519943295)) < 1e-6 &&
              std::abs(m[14] - std::cos(.17453292519943295)) < 1e-6,
          "rotation midpoint not orthonormal half-angle");
    std::array<float, 80> rows{};
    b = perspective(0, {2, 0, 0});
    check(frameTransforms(a, b, rows), "frame transforms unavailable");
    std::array<float, 4> v{.2f, .1f, 1, 1};
    double x = 0, w = 0;
    for (size_t i = 0; i < 4; i++) {
      x += rows[48 + i] * v[i];
      w += rows[60 + i] * v[i];
    }
    check(std::abs(x / w - .2) < 1e-6, "sky received camera translation");
    std::array<float, 96> extended{};
    check(frameTransforms(a, b, extended),
          "previous-to-mid transform unavailable");
    for (size_t r = 0; r < 4; r++)
      for (size_t c = 0; c < 4; c++) {
        double value = 0;
        for (size_t k = 0; k < 4; k++)
          value += double(extended[16 + r * 4 + k]) * extended[80 + k * 4 + c];
        check(std::abs(value - (r == c ? 1.0 : 0.0)) < 1e-5,
              "previous depth mapping roundtrip failed");
      }
    Matrix ortho{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},
        orthoMoved = ortho;
    orthoMoved[3] = .4f;
    check(midpoint(ortho, orthoMoved, m, origin, perspectiveMode) &&
              !perspectiveMode && std::abs(m[3] - .2) < 1e-6,
          "orthographic midpoint incorrect");
    b = perspective(1.5707963267948966, {0, 0, 0});
    check(!midpoint(a, b, m, origin, perspectiveMode),
          "camera cut was synthesized");
    b = a;
    b[0] *= 2;
    check(!midpoint(a, b, m, origin, perspectiveMode),
          "projection jump was synthesized");
    b.fill(0);
    check(!frameTransforms(a, b, rows), "singular matrix was synthesized");
    std::cout << "passed: translation, rotation, sky, orthographic, "
                 "camera-cut, FOV jump, singular rejection\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
