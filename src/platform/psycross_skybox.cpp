#include "psycross_skybox.hpp"

#include "sf/core/error.hpp"
#include "sf/core/file_io.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace sf::platform::detail {
namespace {

constexpr bool campaign_skyboxes_enabled = false;
enum class SkyboxTheme : std::uint8_t {
  none,
  midnight_paris,
  french_village_night,
  desert_morning,
  desert_storm,
  sandstorm,
  greek_coast_night,
  wewelsburg_fog,
  wewelsburg_night,
  monte_cassino_night,
  french_mountains_night,
  french_mountains_morning,
  paris_outskirts_night,
};

struct TgaImage {
  int width{};
  int height{};
  std::vector<std::uint8_t> rgba;
};

struct SkyboxToneMap {
  float exposure{1.0F};
  float gamma{1.0F};
};

[[nodiscard]] std::uint16_t readLe16(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(bytes[offset])) |
         static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(bytes[offset + 1U]))
             << 8U;
}

[[nodiscard]] TgaImage loadTga(const std::filesystem::path &path) {
  const auto bytes = core::readBinaryFile(path);
  constexpr auto header_size = std::size_t{18U};
  if (bytes.size() < header_size ||
      std::to_integer<std::uint8_t>(bytes[1U]) != 0U ||
      std::to_integer<std::uint8_t>(bytes[2U]) != 2U) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Skybox must be an uncompressed true-color TGA"};
  }
  const auto width = readLe16(bytes, 12U);
  const auto height = readLe16(bytes, 14U);
  const auto bits_per_pixel = std::to_integer<std::uint8_t>(bytes[16U]);
  const auto bytes_per_pixel = static_cast<std::size_t>(bits_per_pixel / 8U);
  const auto identifier_size =
      static_cast<std::size_t>(std::to_integer<std::uint8_t>(bytes[0U]));
  const auto pixel_offset = header_size + identifier_size;
  const auto pixel_count = static_cast<std::size_t>(width) * height;
  if (width == 0U || height == 0U ||
      (bytes_per_pixel != 3U && bytes_per_pixel != 4U) ||
      pixel_count > std::numeric_limits<std::size_t>::max() / 4U ||
      pixel_offset > bytes.size() ||
      pixel_count > (bytes.size() - pixel_offset) / bytes_per_pixel) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Skybox TGA has invalid dimensions or pixel data"};
  }

  TgaImage result{static_cast<int>(width), static_cast<int>(height),
                  std::vector<std::uint8_t>(pixel_count * 4U)};
  const auto descriptor = std::to_integer<std::uint8_t>(bytes[17U]);
  const auto top_origin = (descriptor & 0x20U) != 0U;
  const auto right_origin = (descriptor & 0x10U) != 0U;
  for (auto source_y = std::size_t{}; source_y < height; ++source_y) {
    const auto destination_y = top_origin ? height - source_y - 1U : source_y;
    for (auto source_x = std::size_t{}; source_x < width; ++source_x) {
      const auto destination_x =
          right_origin ? width - source_x - 1U : source_x;
      const auto source =
          pixel_offset + (source_y * width + source_x) * bytes_per_pixel;
      const auto destination = (destination_y * width + destination_x) * 4U;
      result.rgba[destination] =
          std::to_integer<std::uint8_t>(bytes[source + 2U]);
      result.rgba[destination + 1U] =
          std::to_integer<std::uint8_t>(bytes[source + 1U]);
      result.rgba[destination + 2U] =
          std::to_integer<std::uint8_t>(bytes[source]);
      result.rgba[destination + 3U] =
          bytes_per_pixel == 4U
              ? std::to_integer<std::uint8_t>(bytes[source + 3U])
              : 255U;
    }
  }
  return result;
}

[[nodiscard]] constexpr SkyboxTheme
skyboxTheme(std::uint8_t campaign_level) noexcept {
  switch (campaign_level) {
  case 1U:
  case 2U:
    return SkyboxTheme::midnight_paris;
  case 3U:
  case 4U:
    return SkyboxTheme::french_village_night;
  case 5U:
    return SkyboxTheme::desert_morning;
  case 6U:
  case 7U:
    return SkyboxTheme::desert_storm;
  case 8U:
    return SkyboxTheme::sandstorm;
  case 9U:
    return SkyboxTheme::greek_coast_night;
  case 12U:
    return SkyboxTheme::wewelsburg_fog;
  case 13U:
  case 14U:
    return SkyboxTheme::wewelsburg_night;
  case 15U:
  case 16U:
  case 17U:
    return SkyboxTheme::monte_cassino_night;
  case 18U:
    return SkyboxTheme::french_mountains_night;
  case 20U:
    return SkyboxTheme::french_mountains_morning;
  case 21U:
  case 22U:
  case 23U:
    return SkyboxTheme::paris_outskirts_night;
  default:
    return SkyboxTheme::none;
  }
}

[[nodiscard]] constexpr std::string_view
skyboxFilename(SkyboxTheme theme) noexcept {
  using enum SkyboxTheme;
  switch (theme) {
  case midnight_paris:
    return "midnight_paris.tga";
  case french_village_night:
    return "french_village_night.tga";
  case desert_morning:
    return "desert_morning.tga";
  case desert_storm:
    return "desert_storm.tga";
  case sandstorm:
    return "sandstorm.tga";
  case greek_coast_night:
    return "greek_coast_night.tga";
  case wewelsburg_fog:
    return "wewelsburg_fog.tga";
  case wewelsburg_night:
    return "wewelsburg_night.tga";
  case monte_cassino_night:
    return "monte_cassino_night.tga";
  case french_mountains_night:
    return "french_mountains_night.tga";
  case french_mountains_morning:
    return "french_mountains_morning.tga";
  case paris_outskirts_night:
    return "paris_outskirts_night.tga";
  case none:
    return {};
  }
  return {};
}

[[nodiscard]] constexpr SkyboxToneMap
skyboxToneMap(SkyboxTheme theme) noexcept {
  using enum SkyboxTheme;
  switch (theme) {
  case midnight_paris:
    return {1.25F, 0.65F};
  case french_village_night:
    return {1.08F, 0.82F};
  case greek_coast_night:
  case wewelsburg_night:
  case monte_cassino_night:
    return {1.12F, 0.78F};
  default:
    return {};
  }
}

[[nodiscard]] constexpr PsyCrossSkyboxFogStyle
skyboxFogStyle(SkyboxTheme theme) noexcept {
  using enum SkyboxTheme;
  switch (theme) {
  case midnight_paris: return {13U, 21U, 34U, 2.25F, true};
  case french_village_night: return {11U, 18U, 27U, 2.30F, true};
  case desert_morning: return {164U, 151U, 126U, 1.70F, true};
  case desert_storm: return {173U, 129U, 70U, 2.65F, true};
  case sandstorm: return {149U, 100U, 48U, 3.20F, true};
  case greek_coast_night: return {10U, 20U, 35U, 2.35F, true};
  case wewelsburg_fog: return {91U, 104U, 111U, 2.70F, true};
  case wewelsburg_night: return {12U, 21U, 35U, 2.40F, true};
  case monte_cassino_night: return {20U, 24U, 33U, 2.30F, true};
  case french_mountains_night: return {12U, 22U, 37U, 2.40F, true};
  case french_mountains_morning: return {96U, 111U, 133U, 1.80F, true};
  case paris_outskirts_night: return {10U, 17U, 28U, 2.35F, true};
  case none: return {};
  }
  return {};
}

void applySkyboxToneMap(TgaImage &image, SkyboxToneMap tone_map) {
  if (tone_map.exposure == 1.0F && tone_map.gamma == 1.0F)
    return;
  const auto map_channel = [tone_map](std::uint8_t channel) {
    const auto normalized = static_cast<float>(channel) / 255.0F;
    const auto mapped =
        std::pow(normalized, tone_map.gamma) * tone_map.exposure;
    return static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(mapped * 255.0F + 0.5F), 0, 255));
  };
  for (std::size_t pixel{}; pixel < image.rgba.size(); pixel += 4U) {
    image.rgba[pixel] = map_channel(image.rgba[pixel]);
    image.rgba[pixel + 1U] = map_channel(image.rgba[pixel + 1U]);
    image.rgba[pixel + 2U] = map_channel(image.rgba[pixel + 2U]);
  }
}

[[nodiscard]] std::filesystem::path skyboxPath(std::string_view filename) {
  using SdlPath = std::unique_ptr<char, decltype(&SDL_free)>;
  SdlPath base{SDL_GetBasePath(), SDL_free};
  if (!base) {
    throw core::Error{core::ErrorCode::io,
                      "Cannot resolve the executable directory"};
  }
  return std::filesystem::path{base.get()} / "assets" / "skyboxes" / filename;
}

} // namespace

PsyCrossSkybox::PsyCrossSkybox() = default;

PsyCrossSkybox::~PsyCrossSkybox() {
  GR_SetGuestSkybox(0U, 0, 0, 0);
  if (texture_ != 0U) {
    GR_DestroyTexture(texture_);
  }
}

void PsyCrossSkybox::setLevel(std::uint8_t campaign_level) {
  const auto next_theme = skyboxTheme(campaign_level);
  if (theme_ == static_cast<std::uint8_t>(next_theme)) {
    return;
  }
  if (!campaign_skyboxes_enabled) {
    GR_SetGuestSkybox(0U, 0, 0, 0);
    if (texture_ != 0U) {
      GR_DestroyTexture(texture_);
    }
    texture_ = 0U;
    width_ = 0;
    height_ = 0;
    theme_ = static_cast<std::uint8_t>(next_theme);
    return;
  }

  if (next_theme == SkyboxTheme::none) {
    GR_SetGuestSkybox(0U, 0, 0, 0);
    if (texture_ != 0U) {
      GR_DestroyTexture(texture_);
    }
    texture_ = 0U;
    width_ = 0;
    height_ = 0;
    theme_ = static_cast<std::uint8_t>(SkyboxTheme::none);
    return;
  }

  auto image = loadTga(skyboxPath(skyboxFilename(next_theme)));
  applySkyboxToneMap(image, skyboxToneMap(next_theme));
  const auto new_texture = GR_CreateRGBATexture(
      image.width, image.height, const_cast<u_char *>(image.rgba.data()));
  if (new_texture == 0U) {
    throw core::Error{core::ErrorCode::io,
                      "Cannot create the campaign skybox texture"};
  }

  GR_SetGuestSkybox(0U, 0, 0, 0);
  if (texture_ != 0U) {
    GR_DestroyTexture(texture_);
  }
  texture_ = new_texture;
  width_ = image.width;
  height_ = image.height;
  theme_ = static_cast<std::uint8_t>(next_theme);
  GR_SetGuestSkybox(texture_, width_, height_, 1);
}

void PsyCrossSkybox::setView(float yaw_radians, float pitch_radians,
                             float vertical_fov_radians, bool valid) noexcept {
  if (valid) {
    GR_SetGuestSkyboxView(yaw_radians, pitch_radians, vertical_fov_radians);
  }
}

PsyCrossSkyboxFogStyle PsyCrossSkybox::fogStyle() const noexcept {
  return skyboxFogStyle(static_cast<SkyboxTheme>(theme_));
}

} // namespace sf::platform::detail
