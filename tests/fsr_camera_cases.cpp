#include "fsr_camera.h"
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool value) {
  if (!value)
    throw std::runtime_error("FSR camera case failed");
}
void close(float a, float b, float tolerance = 0.0001f) {
  require(std::abs(a - b) < tolerance);
}
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--stdin") {
      unsigned index = 0;
      while (true) {
        ldb_camera::Matrix vp{};
        if (!(std::cin >> vp[0]))
          break;
        for (int i = 1; i < 16; ++i)
          if (!(std::cin >> vp[i]))
            throw std::runtime_error("Truncated matrix");
        ldb_fsr::Camera camera;
        if (!ldb_fsr::cameraFromViewProjection(vp, 0.0254f, camera))
          throw std::runtime_error("Captured camera rejected");
        std::cout << "{\"index\":" << index++
                  << ",\"near\":" << camera.nearPlane
                  << ",\"far\":" << camera.farPlane
                  << ",\"vertical_fov_radians\":" << camera.verticalFov
                  << ",\"position\":[" << camera.position[0] << ','
                  << camera.position[1] << ',' << camera.position[2] << "]}\n";
      }
      require(index > 0);
      return 0;
    }
    require(argc == 1);
    const float near = 0.5f, far = 100.f, fov = 1.2f, aspect = 1.7f;
    unsigned positive = 0, negative = 0;
    for (float yaw : {0.f, 0.7f, -1.1f})
      for (float pitch : {0.f, 0.3f}) {
        float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch),
              sp = std::sin(pitch);
        std::array<float, 3> pos{3.4f, -5.6f, 7.8f};
        std::array<float, 3> right{cy, 0, -sy}, up{-sy * sp, cp, -cy * sp},
            forward{sy * cp, sp, cy * cp};
        ldb_camera::Matrix view{}, projection{}, vp{};
        for (int r = 0; r < 3; ++r) {
          auto axis = r == 0 ? right : r == 1 ? up : forward;
          for (int c = 0; c < 3; ++c)
            view[r * 4 + c] = axis[c];
          view[r * 4 + 3] =
              -(axis[0] * pos[0] + axis[1] * pos[1] + axis[2] * pos[2]);
        }
        view[15] = 1;
        projection[0] = 1 / (std::tan(fov / 2) * aspect);
        projection[5] = 1 / std::tan(fov / 2);
        projection[10] = far / (far - near);
        projection[11] = -far * near / (far - near);
        projection[14] = 1;
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k)
              vp[r * 4 + c] += projection[r * 4 + k] * view[k * 4 + c];
        for (float scale : {1.f, 2.f}) {
          auto scaled = vp;
          for (auto &v : scaled)
            v *= scale;
          ldb_fsr::Camera camera;
          require(ldb_fsr::cameraFromViewProjection(scaled, 0.0254f, camera));
          close(camera.nearPlane, near);
          close(camera.farPlane, far, 0.01f);
          close(camera.verticalFov, fov);
          close(camera.metersPerUnit, 0.0254f);
          for (int i = 0; i < 3; ++i) {
            close(camera.position[i], pos[i]);
            close(camera.right[i], right[i]);
            close(camera.up[i], up[i]);
            close(camera.forward[i], forward[i]);
          }
          ++positive;
        }
      }
    auto reject = [&](ldb_camera::Matrix matrix, float meters = 1.f) {
      ldb_fsr::Camera camera;
      camera.nearPlane = 123;
      require(!ldb_fsr::cameraFromViewProjection(matrix, meters, camera));
      close(camera.nearPlane, 123);
      ++negative;
    };
    ldb_camera::Matrix p{};
    p[0] = p[5] = 2;
    p[10] = far / (far - near);
    p[11] = -far * near / (far - near);
    p[14] = 1;
    reject({});
    ldb_camera::Matrix identity{};
    identity[0] = identity[5] = identity[10] = identity[15] = 1;
    reject(identity);
    auto invalid = p;
    invalid[0] = std::numeric_limits<float>::quiet_NaN();
    reject(invalid);
    reject(p, 0);
    reject(p, std::numeric_limits<float>::infinity());
    invalid = p;
    invalid[2] = 0.2f;
    reject(invalid); // Off-axis/jitter outside this profile.
    invalid = p;
    invalid[10] = 1;
    invalid[11] = -near;
    reject(invalid); // Infinite far.
    invalid = p;
    invalid[10] = -near / (far - near);
    invalid[11] = near * far / (far - near);
    reject(invalid); // Reverse Z.
    std::cout << "{\"positive\":" << positive << ",\"negative\":" << negative
              << ",\"status\":\"passed\"}\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
