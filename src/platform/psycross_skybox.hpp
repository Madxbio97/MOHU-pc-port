#pragma once

#include <PsyX/PsyX_render.h>

#include <cstdint>

namespace sf::platform::detail {

struct PsyCrossSkyboxFogStyle {
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};
  float density{1.0F};
  bool valid{};
};

class PsyCrossSkybox final {
public:
  PsyCrossSkybox();
  ~PsyCrossSkybox();

  PsyCrossSkybox(const PsyCrossSkybox &) = delete;
  PsyCrossSkybox &operator=(const PsyCrossSkybox &) = delete;
  PsyCrossSkybox(PsyCrossSkybox &&) = delete;
  PsyCrossSkybox &operator=(PsyCrossSkybox &&) = delete;

  void setLevel(std::uint8_t campaign_level);
  void setView(float yaw_radians, float pitch_radians,
               float vertical_fov_radians, bool valid) noexcept;
  [[nodiscard]] PsyCrossSkyboxFogStyle fogStyle() const noexcept;

private:
  TextureID texture_{};
  int width_{};
  int height_{};
  std::uint8_t theme_{};
};

} // namespace sf::platform::detail
