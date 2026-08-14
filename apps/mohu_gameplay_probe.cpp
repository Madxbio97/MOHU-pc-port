#include "mohu/runtime.hpp"

#if defined(MOHU_GAMEPLAY_RENDER_PROBE)
#include "psycross_guest_gpu.hpp"

#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>
#include <PsyX/common/glad.h>
#include <SDL.h>
#include <psx/libgpu.h>

extern SDL_Window *g_window;
extern void PsyX_TakeScreenshot();
extern GLuint g_glNativeFramebuffer;
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

#if defined(MOHU_GAMEPLAY_RENDER_PROBE)
enum class RenderProbeMode {
  disabled,
  baseline,
  baseline_no_depth,
  baseline_no_pgxp,
  baseline_exact_cpu,
  quality,
};

[[nodiscard]] std::optional<RenderProbeMode>
parseRenderProbeMode(std::string_view option) noexcept {
  if (option == "--render-baseline") {
    return RenderProbeMode::baseline;
  }
  if (option == "--render-no-pgxp") {
    return RenderProbeMode::baseline_no_pgxp;
  }
  if (option == "--render-no-depth") {
    return RenderProbeMode::baseline_no_depth;
  }
  if (option == "--render-exact-cpu") {
    return RenderProbeMode::baseline_exact_cpu;
  }
  if (option == "--render-quality") {
    return RenderProbeMode::quality;
  }
  return std::nullopt;
}

class RenderProbeSession final {
public:
  RenderProbeSession() = default;
  RenderProbeSession(const RenderProbeSession &) = delete;
  RenderProbeSession &operator=(const RenderProbeSession &) = delete;
  ~RenderProbeSession() {
    if (active_) {
      PsyX_Shutdown();
    }
  }

  void start(RenderProbeMode mode) {
    if (mode == RenderProbeMode::disabled || active_) {
      return;
    }
    SDL_SetMainReady();
    g_cfg_framebufferFeedback = 0;
    g_cfg_vblankThread = 0;
    g_cfg_composedGuestScanout = 1;
    g_cfg_renderWidth = 1280;
    g_cfg_renderHeight = 720;
    g_cfg_aspectMode = PSYX_ASPECT_ADAPTIVE;
    g_cfg_msaaSamples = 0;
    g_cfg_bilinearFiltering = 1;
    g_cfg_trilinearFiltering = mode == RenderProbeMode::quality ? 1 : 0;
    g_cfg_anisotropicFiltering = mode == RenderProbeMode::quality ? 1 : 0;
    g_cfg_smaa = mode == RenderProbeMode::quality ? 1 : 0;
    g_cfg_smaaFinalFrame = g_cfg_smaa;
    g_cfg_pgxpTextureCorrection = 1;
    const bool depth_enabled = mode != RenderProbeMode::baseline_no_depth &&
                               mode != RenderProbeMode::baseline_exact_cpu;
    g_cfg_pgxpZBuffer = depth_enabled ? 1 : 0;
    char title[] = "MOHU hidden gameplay render probe";
    PsyX_Initialise(title, g_cfg_renderWidth, g_cfg_renderHeight, 0);
    if (g_window == nullptr) {
      throw std::runtime_error{"PsyCross did not create the probe window"};
    }
    SDL_HideWindow(g_window);
    PsyX_EnableSwapInterval(0);
    PsyX_SetFrameLimit(0);
    GR_EnableDepth(depth_enabled ? 1 : 0);
    active_ = true;
    start_counter_ = SDL_GetPerformanceCounter();
    counter_frequency_ = SDL_GetPerformanceFrequency();
  }

  void render(const mohu::Runtime &runtime, bool capture) {
    render(runtime.gpuCommands(), runtime.gpuProjections(),
           runtime.gpuProjectionIdentities(), runtime.gpuProjectionCatalog(),
           runtime.gpuDisplayState(), runtime.gpuCommandBufferEpoch(),
           capture);
  }

  void render(std::span<const std::uint32_t> words,
              std::span<const sf::psx::GteProjectedVertex> projections,
              std::span<const std::uint64_t> projection_identities,
              std::span<const sf::psx::GteProjectedVertex> projection_catalog,
              const mohu::GpuDisplayState &display,
              std::uint64_t command_buffer_epoch, bool capture,
              bool present = true, bool measure = true) {
    if (!active_) {
      return;
    }
    const auto render_start =
        measure ? SDL_GetPerformanceCounter() : std::uint64_t{};
    GR_SetGuestDisplayGeometry(static_cast<int>(display.width),
                               static_cast<int>(display.height));
    if (present) {
      gpu_.presentDisplay(display.x, display.y, display.width, display.height,
                          display.enabled, display.rgb24, display.interlaced);
    }
    if (capture) {
      PsyX_TakeScreenshot();
      glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glNativeFramebuffer);
    }
    GR_BeginGuestProjectionEpoch(
        ++projection_epoch_, static_cast<int>(display.width),
        static_cast<int>(display.height), display.rgb24 ? 1 : 0,
        display.interlaced ? 1 : 0);
    GR_BeginGuestSubmit();
    gpu_.submit(words, projections, projection_identities,
                command_buffer_epoch, projection_catalog);
    static_cast<void>(PsyX_BeginScene());
    DrawSync(0);
    PsyX_EndScene();
    if (measure) {
      render_ticks_ += SDL_GetPerformanceCounter() - render_start;
      ++rendered_frames_;
    }
  }

  void captureCurrentFrame() {
    if (!active_) {
      return;
    }
    PsyX_TakeScreenshot();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_glNativeFramebuffer);
  }

  [[nodiscard]] std::uint64_t polygonPrimitives() const noexcept {
    return gpu_.polygonPrimitives();
  }

  void printSummary(RenderProbeMode mode) const {
    if (!active_) {
      return;
    }
    glFinish();
    const auto elapsed_ticks = SDL_GetPerformanceCounter() - start_counter_;
    const auto seconds = counter_frequency_ != 0U
                             ? static_cast<double>(elapsed_ticks) /
                                   static_cast<double>(counter_frequency_)
                             : 0.0;
    const auto fps =
        seconds > 0.0 ? static_cast<double>(rendered_frames_) / seconds : 0.0;
    const auto render_seconds =
        counter_frequency_ != 0U ? static_cast<double>(render_ticks_) /
                                       static_cast<double>(counter_frequency_)
                                 : 0.0;
    const auto render_fps =
        render_seconds > 0.0
            ? static_cast<double>(rendered_frames_) / render_seconds
            : 0.0;
    std::cout
        << "GL render probe: mode="
        << (mode == RenderProbeMode::quality              ? "quality"
            : mode == RenderProbeMode::baseline_exact_cpu ? "baseline-exact-cpu"
            : mode == RenderProbeMode::baseline_no_depth  ? "baseline-no-depth"
            : mode == RenderProbeMode::baseline_no_pgxp   ? "baseline-no-pgxp"
                                                          : "baseline")
        << " frames=" << rendered_frames_ << " seconds=" << seconds
        << " fps=" << fps << " render_seconds=" << render_seconds
        << " render_fps=" << render_fps
        << " polygons=" << gpu_.polygonPrimitives()
        << " candidates=" << gpu_.preciseCandidates()
        << " precise=" << gpu_.precisePrimitives()
        << " partial=" << gpu_.partialProjectionPrimitives()
        << " coherence_fallback=" << gpu_.coherenceFallbackPrimitives()
        << " unsupported=" << gpu_.unsupportedCommands()
        << " reject_missing=" << gpu_.missingProjectionPrimitives()
        << " reject_nonfinite=" << gpu_.nonfiniteProjectionPrimitives()
        << " reject_packet=" << gpu_.packetMismatchPrimitives()
        << " reject_hazard=" << gpu_.projectionHazardPrimitives()
        << " reject_screen=" << gpu_.screenMismatchPrimitives()
        << " reject_reprojection=" << gpu_.reprojectionMismatchPrimitives()
        << " reject_camera=" << gpu_.cameraMismatchPrimitives()
        << " identity_recovered_vertices=" << gpu_.identityRecoveredVertices()
        << " identity_recovered_primitives="
        << gpu_.identityRecoveredPrimitives()
        << " identity_conflict_primitives=" << gpu_.identityConflictPrimitives()
        << " highres_presents=" << gpu_.highResolutionPresents()
        << " fallback_presents=" << gpu_.fallbackPresents()
        << " sync_vram_readbacks=" << GR_GetSynchronousVRAMReadbackCount()
        << " packs=" << GR_GetGuestVRAMPackCount()
        << " pack_pixels=" << GR_GetGuestVRAMPackPixels()
        << " seeds=" << GR_GetGuestSeedCount()
        << " seed_pixels=" << GR_GetGuestSeedPixels()
        << " captures=" << GR_GetGuestCaptureCount()
        << " capture_pixels=" << GR_GetGuestCapturePixels() << '\n';
    std::cout << "PGXP missing opcode histogram:";
    for (std::uint8_t opcode = 0x20U; opcode < 0x40U; ++opcode) {
      const auto count = gpu_.missingProjectionPrimitives(opcode);
      if (count != 0U) {
        std::cout << " 0x" << std::hex << static_cast<unsigned>(opcode)
                  << std::dec << '=' << count;
      }
    }
    std::cout << '\n';
  }

private:
  sf::platform::detail::PsyCrossGuestGpu gpu_;
  std::uint64_t projection_epoch_{};
  std::uint64_t rendered_frames_{};
  std::uint64_t render_ticks_{};
  std::uint64_t start_counter_{};
  std::uint64_t counter_frequency_{};
  bool active_{};
};
#endif

constexpr std::uint32_t level_pad_one_buffer = 0x800a04d8U;
constexpr std::uint32_t level_buttons_one = 0x800a04b4U;
constexpr std::uint32_t level_buttons_one_alternate = 0x800a04bcU;
constexpr std::uint16_t neutral_buttons = 0xffffU;
constexpr std::uint16_t cross_buttons = 0xbfffU;
constexpr std::uint16_t dpad_up_mask = 0x0010U;
constexpr std::uint16_t circle_mask = 0x2000U;
constexpr std::uint32_t level_dpad_up_mask = 0x1000U;
constexpr std::uint32_t level_circle_mask = 0x0020U;
constexpr std::uint64_t default_maximum_frames = 48'000U;
constexpr std::uint64_t menu_pulse_start_frame = 6'000U;
constexpr std::uint64_t menu_pulse_period_frames = 180U;
constexpr std::uint64_t menu_pulse_hold_frames = 30U;
constexpr std::uint64_t default_gameplay_frames = 3'600U;
constexpr std::uint64_t gameplay_stall_frames = 900U;
constexpr std::uint64_t snapshot_period_frames = 600U;
constexpr std::array<std::uint8_t, 4U> centered_axes{128U, 128U, 128U, 128U};

struct DiscExtent {
  std::string path;
  std::uint32_t first_lba{};
  std::uint32_t sector_count{};
};

struct PadSample {
  std::uint8_t status{0xffU};
  std::uint8_t id{0xffU};
  std::uint16_t buttons{neutral_buttons};
};

enum class ProbeStage {
  boot,
  menu_navigation,
  mission_loading,
  gameplay,
};

constexpr std::array<char, 8U> map_checkpoint_magic{'M', 'O', 'H', 'U',
                                                    'M', 'A', 'P', '\0'};
constexpr std::uint32_t map_checkpoint_version = 1U;
constexpr std::uint64_t maximum_checkpoint_frames = 100'000U;
constexpr std::uint32_t maximum_checkpoint_words_per_frame = 1'000'000U;

template <typename T>
void writeCheckpointValue(std::ostream &stream, const T &value) {
  static_assert(std::is_trivially_copyable_v<T>);
  stream.write(reinterpret_cast<const char *>(&value),
               static_cast<std::streamsize>(sizeof(value)));
  if (!stream) {
    throw std::runtime_error{"Cannot write map checkpoint"};
  }
}

template <typename T> void readCheckpointValue(std::istream &stream, T &value) {
  static_assert(std::is_trivially_copyable_v<T>);
  stream.read(reinterpret_cast<char *>(&value),
              static_cast<std::streamsize>(sizeof(value)));
  if (!stream) {
    throw std::runtime_error{"Truncated map checkpoint"};
  }
}

template <typename T>
void writeCheckpointSpan(std::ostream &stream, std::span<const T> values) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (values.empty()) {
    return;
  }
  stream.write(reinterpret_cast<const char *>(values.data()),
               static_cast<std::streamsize>(values.size_bytes()));
  if (!stream) {
    throw std::runtime_error{"Cannot write map checkpoint payload"};
  }
}

template <typename T>
void readCheckpointSpan(std::istream &stream, std::span<T> values) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (values.empty()) {
    return;
  }
  stream.read(reinterpret_cast<char *>(values.data()),
              static_cast<std::streamsize>(values.size_bytes()));
  if (!stream) {
    throw std::runtime_error{"Truncated map checkpoint payload"};
  }
}

class MapCheckpointWriter final {
public:
  explicit MapCheckpointWriter(std::filesystem::path path)
      : path_(std::move(path)) {
    static_assert(std::is_trivially_copyable_v<sf::psx::GteProjectedVertex>);
    if (path_.empty()) {
      throw std::invalid_argument{"Empty map checkpoint path"};
    }
    std::error_code error;
    if (std::filesystem::exists(path_, error) || error) {
      throw std::runtime_error{"Map checkpoint already exists or is invalid: " +
                               path_.string()};
    }
    temporary_path_ = path_;
    temporary_path_ += ".tmp";
    stream_.open(temporary_path_, std::ios::binary | std::ios::trunc);
    if (!stream_) {
      throw std::runtime_error{"Cannot create map checkpoint: " +
                               temporary_path_.string()};
    }
    try {
      stream_.write(map_checkpoint_magic.data(),
                    static_cast<std::streamsize>(map_checkpoint_magic.size()));
      writeCheckpointValue(stream_, map_checkpoint_version);
      const auto projected_size =
          static_cast<std::uint32_t>(sizeof(sf::psx::GteProjectedVertex));
      writeCheckpointValue(stream_, projected_size);
      for (std::size_t field{}; field < 4U; ++field) {
        writeCheckpointValue(stream_, std::uint64_t{});
      }
    } catch (...) {
      stream_.close();
      static_cast<void>(std::filesystem::remove(temporary_path_, error));
      throw;
    }
  }

  ~MapCheckpointWriter() {
    if (finalized_) {
      return;
    }
    stream_.close();
    std::error_code error;
    static_cast<void>(std::filesystem::remove(temporary_path_, error));
  }

  void append(const mohu::Runtime &runtime, std::uint64_t source_frame,
              ProbeStage stage) {
    const auto words = runtime.gpuCommands();
    const auto projections = runtime.gpuProjections();
    const auto identities = runtime.gpuProjectionIdentities();
    if (words.size() > maximum_checkpoint_words_per_frame ||
        (!projections.empty() && projections.size() != words.size()) ||
        (!identities.empty() && identities.size() != words.size()) ||
        frame_count_ >= maximum_checkpoint_frames) {
      throw std::runtime_error{"Unaligned or excessive GPU checkpoint frame"};
    }

    const auto projection_count = static_cast<std::uint32_t>(
        std::ranges::count_if(projections, [](const auto &projected) {
          return projected.valid;
        }));
    const auto identity_count =
        static_cast<std::uint32_t>(std::ranges::count_if(
            identities, [](std::uint64_t identity) { return identity != 0U; }));
    const auto word_count = static_cast<std::uint32_t>(words.size());
    const auto display = runtime.gpuDisplayState();
    const auto flags = static_cast<std::uint8_t>(
        (display.enabled ? 1U : 0U) | (display.rgb24 ? 2U : 0U) |
        (display.interlaced ? 4U : 0U));
    const auto encoded_stage = static_cast<std::uint8_t>(stage);

    writeCheckpointValue(stream_, source_frame);
    writeCheckpointValue(stream_, runtime.gpuCommandBufferEpoch());
    writeCheckpointValue(stream_, word_count);
    writeCheckpointValue(stream_, projection_count);
    writeCheckpointValue(stream_, identity_count);
    writeCheckpointValue(stream_, display.x);
    writeCheckpointValue(stream_, display.y);
    writeCheckpointValue(stream_, display.width);
    writeCheckpointValue(stream_, display.height);
    writeCheckpointValue(stream_, flags);
    writeCheckpointValue(stream_, encoded_stage);
    writeCheckpointValue(stream_, std::uint16_t{});
    writeCheckpointSpan(stream_, words);

    for (std::size_t index{}; index < projections.size(); ++index) {
      if (!projections[index].valid) {
        continue;
      }
      const auto word_index = static_cast<std::uint32_t>(index);
      writeCheckpointValue(stream_, word_index);
      writeCheckpointValue(stream_, projections[index]);
    }
    for (std::size_t index{}; index < identities.size(); ++index) {
      if (identities[index] == 0U) {
        continue;
      }
      const auto word_index = static_cast<std::uint32_t>(index);
      writeCheckpointValue(stream_, word_index);
      writeCheckpointValue(stream_, identities[index]);
    }

    if (frame_count_ == 0U) {
      first_source_frame_ = source_frame;
    }
    if (stage == ProbeStage::gameplay) {
      if (gameplay_frame_count_ == 0U) {
        first_gameplay_frame_ = source_frame;
      }
      ++gameplay_frame_count_;
    }
    ++frame_count_;
  }

  void finalize() {
    if (finalized_) {
      return;
    }
    if (frame_count_ == 0U || gameplay_frame_count_ == 0U) {
      throw std::runtime_error{
          "Map checkpoint contains no rendered gameplay frames"};
    }
    const auto end = stream_.tellp();
    stream_.seekp(16, std::ios::beg);
    writeCheckpointValue(stream_, frame_count_);
    writeCheckpointValue(stream_, first_source_frame_);
    writeCheckpointValue(stream_, first_gameplay_frame_);
    writeCheckpointValue(stream_, gameplay_frame_count_);
    stream_.seekp(end);
    stream_.flush();
    if (!stream_) {
      throw std::runtime_error{"Cannot finalize map checkpoint"};
    }
    stream_.close();
    std::error_code error;
    std::filesystem::rename(temporary_path_, path_, error);
    if (error) {
      throw std::runtime_error{"Cannot publish map checkpoint: " +
                               error.message()};
    }
    finalized_ = true;
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }
  [[nodiscard]] std::uint64_t frameCount() const noexcept {
    return frame_count_;
  }
  [[nodiscard]] std::uint64_t gameplayFrameCount() const noexcept {
    return gameplay_frame_count_;
  }

private:
  std::filesystem::path path_;
  std::filesystem::path temporary_path_;
  std::ofstream stream_;
  std::uint64_t frame_count_{};
  std::uint64_t first_source_frame_{};
  std::uint64_t first_gameplay_frame_{};
  std::uint64_t gameplay_frame_count_{};
  bool finalized_{};
};

struct MapReplayFrame {
  std::vector<std::uint32_t> words;
  std::vector<sf::psx::GteProjectedVertex> projections;
  std::vector<std::uint64_t> projection_identities;
  mohu::GpuDisplayState display;
  std::uint64_t source_frame{};
  std::uint64_t command_buffer_epoch{};
  ProbeStage stage{ProbeStage::boot};
};

class MapCheckpointReader final {
public:
  explicit MapCheckpointReader(const std::filesystem::path &path)
      : stream_(path, std::ios::binary) {
    if (!stream_) {
      throw std::runtime_error{"Cannot open map checkpoint: " + path.string()};
    }
    std::array<char, map_checkpoint_magic.size()> magic{};
    stream_.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    std::uint32_t version{};
    std::uint32_t projected_size{};
    readCheckpointValue(stream_, version);
    readCheckpointValue(stream_, projected_size);
    readCheckpointValue(stream_, frame_count_);
    readCheckpointValue(stream_, first_source_frame_);
    readCheckpointValue(stream_, first_gameplay_frame_);
    readCheckpointValue(stream_, gameplay_frame_count_);
    if (magic != map_checkpoint_magic || version != map_checkpoint_version ||
        projected_size != sizeof(sf::psx::GteProjectedVertex) ||
        frame_count_ == 0U || frame_count_ > maximum_checkpoint_frames ||
        first_source_frame_ != 1U || gameplay_frame_count_ == 0U ||
        gameplay_frame_count_ > frame_count_ ||
        first_gameplay_frame_ < first_source_frame_) {
      throw std::runtime_error{"Invalid or incompatible map checkpoint"};
    }
  }

  [[nodiscard]] bool readFrame(MapReplayFrame &frame) {
    if (frames_read_ == frame_count_) {
      return false;
    }
    std::uint32_t word_count{};
    std::uint32_t projection_count{};
    std::uint32_t identity_count{};
    std::uint8_t flags{};
    std::uint8_t encoded_stage{};
    std::uint16_t reserved{};
    readCheckpointValue(stream_, frame.source_frame);
    readCheckpointValue(stream_, frame.command_buffer_epoch);
    readCheckpointValue(stream_, word_count);
    readCheckpointValue(stream_, projection_count);
    readCheckpointValue(stream_, identity_count);
    readCheckpointValue(stream_, frame.display.x);
    readCheckpointValue(stream_, frame.display.y);
    readCheckpointValue(stream_, frame.display.width);
    readCheckpointValue(stream_, frame.display.height);
    readCheckpointValue(stream_, flags);
    readCheckpointValue(stream_, encoded_stage);
    readCheckpointValue(stream_, reserved);
    if (word_count > maximum_checkpoint_words_per_frame ||
        projection_count > word_count || identity_count > word_count ||
        encoded_stage > static_cast<std::uint8_t>(ProbeStage::gameplay) ||
        (flags & ~std::uint8_t{7U}) != 0U || reserved != 0U ||
        frame.display.width == 0U || frame.display.height == 0U ||
        frame.display.width > 1024U || frame.display.height > 1024U ||
        (frames_read_ != 0U && frame.source_frame <= previous_source_frame_)) {
      throw std::runtime_error{"Invalid map checkpoint frame"};
    }
    frame.stage = static_cast<ProbeStage>(encoded_stage);
    frame.display.enabled = (flags & 1U) != 0U;
    frame.display.rgb24 = (flags & 2U) != 0U;
    frame.display.interlaced = (flags & 4U) != 0U;
    frame.words.resize(word_count);
    readCheckpointSpan(stream_, std::span{frame.words});

    frame.projections.clear();
    if (projection_count != 0U) {
      frame.projections.resize(word_count);
      std::uint32_t previous_index{};
      for (std::uint32_t item{}; item < projection_count; ++item) {
        std::uint32_t index{};
        sf::psx::GteProjectedVertex projected{};
        readCheckpointValue(stream_, index);
        readCheckpointValue(stream_, projected);
        if (index >= word_count || (item != 0U && index <= previous_index) ||
            !projected.valid) {
          throw std::runtime_error{"Invalid sparse projection checkpoint"};
        }
        frame.projections[index] = projected;
        previous_index = index;
      }
    }

    frame.projection_identities.clear();
    if (identity_count != 0U) {
      frame.projection_identities.resize(word_count);
      std::uint32_t previous_index{};
      for (std::uint32_t item{}; item < identity_count; ++item) {
        std::uint32_t index{};
        std::uint64_t identity{};
        readCheckpointValue(stream_, index);
        readCheckpointValue(stream_, identity);
        if (index >= word_count || identity == 0U ||
            (item != 0U && index <= previous_index)) {
          throw std::runtime_error{"Invalid sparse identity checkpoint"};
        }
        frame.projection_identities[index] = identity;
        previous_index = index;
      }
    }

    previous_source_frame_ = frame.source_frame;
    ++frames_read_;
    return true;
  }

  void validateEnd() {
    if (frames_read_ != frame_count_ ||
        stream_.peek() != std::char_traits<char>::eof()) {
      throw std::runtime_error{"Map checkpoint has trailing or missing data"};
    }
  }

  [[nodiscard]] std::uint64_t frameCount() const noexcept {
    return frame_count_;
  }
  [[nodiscard]] std::uint64_t firstSourceFrame() const noexcept {
    return first_source_frame_;
  }
  [[nodiscard]] std::uint64_t firstGameplayFrame() const noexcept {
    return first_gameplay_frame_;
  }
  [[nodiscard]] std::uint64_t gameplayFrameCount() const noexcept {
    return gameplay_frame_count_;
  }

private:
  std::ifstream stream_;
  std::uint64_t frame_count_{};
  std::uint64_t first_source_frame_{};
  std::uint64_t first_gameplay_frame_{};
  std::uint64_t gameplay_frame_count_{};
  std::uint64_t frames_read_{};
  std::uint64_t previous_source_frame_{};
};

struct PgxpPrimitiveCoverage {
  std::uint64_t polygons{};
  std::uint64_t precise_polygons{};
  std::uint64_t partial_polygons{};
  std::uint64_t exact_transform_polygons{};
  std::uint64_t fractional_transform_polygons{};
  std::uint64_t exact_near_plane_polygons{};
};

[[nodiscard]] bool isPolylineTerminator(std::uint32_t word) noexcept {
  return (word & 0xf000f000U) == 0x50005000U;
}

[[nodiscard]] std::size_t
gpuCommandLength(std::span<const std::uint32_t> words) noexcept {
  if (words.empty()) {
    return 0U;
  }
  const auto opcode = static_cast<std::uint8_t>(words.front() >> 24U);
  if ((opcode >= 0x48U && opcode < 0x50U) ||
      (opcode >= 0x58U && opcode < 0x60U)) {
    const auto minimum = opcode < 0x50U ? 4U : 5U;
    if (words.size() < minimum) {
      return 0U;
    }
    const auto end =
        std::find_if(words.begin() + static_cast<std::ptrdiff_t>(minimum - 1U),
                     words.end(), isPolylineTerminator);
    return end == words.end()
               ? 0U
               : static_cast<std::size_t>(end - words.begin()) + 1U;
  }
  if (opcode >= 0xa0U && opcode < 0xc0U) {
    if (words.size() < 3U) {
      return 0U;
    }
    const auto width = (((words[2] & 0xffffU) - 1U) & 0x03ffU) + 1U;
    const auto height = ((((words[2] >> 16U) & 0xffffU) - 1U) & 0x01ffU) + 1U;
    return 3U + (static_cast<std::size_t>(width) * height + 1U) / 2U;
  }
  if (opcode >= 0xc0U && opcode < 0xe0U) {
    return 3U;
  }
  if (opcode == 0x02U) {
    return 3U;
  }
  if (opcode < 0x20U) {
    return 1U;
  }
  constexpr std::array<std::size_t, 24U> polygon_lengths{
      4U, 4U, 4U, 4U, 7U, 7U, 7U, 7U, 5U, 5U, 5U, 5U,
      9U, 9U, 9U, 9U, 6U, 6U, 6U, 6U, 9U, 9U, 9U, 9U};
  if (opcode < 0x38U) {
    return polygon_lengths[opcode - 0x20U];
  }
  if (opcode < 0x3cU) {
    return 8U;
  }
  if (opcode < 0x40U) {
    return 12U;
  }
  if (opcode < 0x48U) {
    return 3U;
  }
  if (opcode < 0x50U) {
    return 0U;
  }
  if (opcode < 0x58U) {
    return 4U;
  }
  if (opcode < 0x60U) {
    return 0U;
  }
  if (opcode < 0x64U) {
    return 3U;
  }
  if (opcode < 0x68U) {
    return 4U;
  }
  if (opcode < 0x6cU) {
    return 2U;
  }
  if (opcode < 0x70U) {
    return 3U;
  }
  if (opcode < 0x74U) {
    return 2U;
  }
  if (opcode < 0x78U) {
    return 3U;
  }
  if (opcode < 0x7cU) {
    return 2U;
  }
  if (opcode < 0x80U) {
    return 3U;
  }
  return opcode < 0xa0U ? 4U : 1U;
}

void accumulatePgxpCoverage(
    std::span<const std::uint32_t> words,
    std::span<const sf::psx::GteProjectedVertex> projections,
    PgxpPrimitiveCoverage &coverage) noexcept {
  std::size_t offset{};
  while (offset < words.size()) {
    const auto remaining = words.subspan(offset);
    const auto length = gpuCommandLength(remaining);
    if (length == 0U || length > remaining.size()) {
      break;
    }
    const auto opcode = static_cast<std::uint8_t>(remaining.front() >> 24U);
    if (opcode >= 0x20U && opcode < 0x40U) {
      ++coverage.polygons;
      const auto vertex_count = (opcode & 0x08U) != 0U ? 4U : 3U;
      std::array<std::size_t, 4U> coordinate_words{};
      if (opcode < 0x24U) {
        coordinate_words = {1U, 2U, 3U, 0U};
      } else if (opcode < 0x28U) {
        coordinate_words = {1U, 3U, 5U, 0U};
      } else if (opcode < 0x2cU) {
        coordinate_words = {1U, 2U, 3U, 4U};
      } else if (opcode < 0x30U) {
        coordinate_words = {1U, 3U, 5U, 7U};
      } else if (opcode < 0x34U) {
        coordinate_words = {1U, 3U, 5U, 0U};
      } else if (opcode < 0x38U) {
        coordinate_words = {1U, 4U, 7U, 0U};
      } else if (opcode < 0x3cU) {
        coordinate_words = {1U, 3U, 5U, 7U};
      } else {
        coordinate_words = {1U, 4U, 7U, 10U};
      }
      auto precise_vertices = std::size_t{};
      auto exact_vertices = std::size_t{};
      auto fractional_vertices = std::size_t{};
      auto exact_near_plane = false;
      for (std::size_t vertex{}; vertex < vertex_count; ++vertex) {
        const auto word = offset + coordinate_words[vertex];
        if (word >= projections.size()) {
          continue;
        }
        const auto &projected = projections[word];
        const auto exact_witness =
            projected.valid && projected.packed_sxy == words[word] &&
            projected.exact_transform && std::isfinite(projected.view_x) &&
            std::isfinite(projected.view_y) &&
            std::isfinite(projected.view_z) &&
            std::isfinite(projected.screen_h) && projected.screen_h > 0.0F;
        if (exact_witness) {
          ++exact_vertices;
          fractional_vertices += projected.fractional_transform ? 1U : 0U;
          exact_near_plane =
              exact_near_plane || projected.view_z <= projected.screen_h * 0.5F;
        }
        if (projected.valid && projected.packed_sxy == words[word] &&
            std::isfinite(projected.view_x) &&
            std::isfinite(projected.view_y) &&
            std::isfinite(projected.view_z) &&
            std::isfinite(projected.screen_x) &&
            std::isfinite(projected.screen_y) &&
            std::isfinite(projected.screen_h) &&
            std::isfinite(projected.screen_offset_x) &&
            std::isfinite(projected.screen_offset_y) &&
            projected.view_z > 0.0F && projected.screen_h > 0.0F) {
          ++precise_vertices;
        }
      }
      coverage.precise_polygons += precise_vertices == vertex_count ? 1U : 0U;
      coverage.partial_polygons +=
          precise_vertices != 0U && precise_vertices != vertex_count ? 1U : 0U;
      coverage.exact_transform_polygons +=
          exact_vertices == vertex_count ? 1U : 0U;
      coverage.fractional_transform_polygons +=
          fractional_vertices == vertex_count ? 1U : 0U;
      coverage.exact_near_plane_polygons +=
          exact_vertices == vertex_count && exact_near_plane ? 1U : 0U;
    }
    offset += length;
  }
}
std::string_view stageName(ProbeStage stage) noexcept {
  switch (stage) {
  case ProbeStage::boot:
    return "boot";
  case ProbeStage::menu_navigation:
    return "menu";
  case ProbeStage::mission_loading:
    return "mission-load";
  case ProbeStage::gameplay:
    return "gameplay";
  }
  return "unknown";
}

DiscExtent findExtent(sf::game::GameDisc &disc, std::string_view path) {
  const auto entry = disc.image().find(std::string{path});
  if (entry.is_directory || entry.size == 0U) {
    throw std::runtime_error{"Invalid first-mission file: " +
                             std::string{path}};
  }
  return DiscExtent{std::string{path}, entry.extent_lba,
                    (entry.size + 2'047U) / 2'048U};
}

bool containsLba(const DiscExtent &extent, std::uint32_t lba) noexcept {
  return lba >= extent.first_lba &&
         static_cast<std::uint64_t>(lba - extent.first_lba) <
             extent.sector_count;
}

bool recentReadsContain(const sf::psx::CdRomState &cd,
                        std::span<const DiscExtent> extents,
                        std::string_view &matched_path) noexcept {
  const auto oldest = static_cast<std::size_t>(
      (cd.recent_lba_cursor + cd.recent_lbas.size() - cd.recent_lba_count) %
      cd.recent_lbas.size());
  for (std::size_t index{}; index < cd.recent_lba_count; ++index) {
    const auto lba = cd.recent_lbas[(oldest + index) % cd.recent_lbas.size()];
    for (const auto &extent : extents) {
      if (containsLba(extent, lba)) {
        matched_path = extent.path;
        return true;
      }
    }
  }
  return false;
}

PadSample readPad(const mohu::Runtime &runtime) noexcept {
  PadSample sample;
  static_cast<void>(runtime.cpu().read8(level_pad_one_buffer, sample.status));
  static_cast<void>(runtime.cpu().read8(level_pad_one_buffer + 1U, sample.id));
  static_cast<void>(
      runtime.cpu().read16(level_pad_one_buffer + 2U, sample.buttons));
  return sample;
}

bool isLiveLevelPad(const PadSample &sample) noexcept {
  return sample.status == 0U && (sample.id == 0x41U || sample.id == 0x73U);
}

std::uint32_t readLevelButtons(const mohu::Runtime &runtime) noexcept {
  std::uint32_t primary{};
  std::uint32_t alternate{};
  static_cast<void>(runtime.cpu().read32(level_buttons_one, primary));
  static_cast<void>(
      runtime.cpu().read32(level_buttons_one_alternate, alternate));
  return primary | alternate;
}

std::uint16_t scriptedButtons(std::uint64_t frame, ProbeStage stage,
                              std::uint64_t gameplay_frame) noexcept {
  if (stage == ProbeStage::boot && frame < menu_pulse_start_frame) {
    return neutral_buttons;
  }
  if (stage != ProbeStage::gameplay) {
    const auto pulse_offset =
        (frame - menu_pulse_start_frame) % menu_pulse_period_frames;
    return pulse_offset < menu_pulse_hold_frames ? cross_buttons
                                                 : neutral_buttons;
  }

  auto buttons = static_cast<std::uint16_t>(neutral_buttons & ~dpad_up_mask);
  // Keep forward movement continuous and produce a clean Circle/fire edge
  // once per second. This exercises the mission controller without relying on
  // host timing or SDL event injection.
  if (gameplay_frame % 60U < 12U) {
    buttons = static_cast<std::uint16_t>(buttons & ~circle_mask);
  }
  return buttons;
}

void printSnapshot(const mohu::Runtime &runtime, std::uint64_t frame,
                   ProbeStage stage, std::uint64_t audio_frames) {
  const auto &cpu = runtime.cpu().state();
  const auto &stats = runtime.stats();
  const auto display = runtime.gpuDisplayState();
  const auto cd = runtime.machine().cdrom().captureState();
  const auto pad = readPad(runtime);
  const auto interrupt_status = runtime.machine().interrupts().status();
  const auto interrupt_mask = runtime.machine().interrupts().mask();
  const auto &spu = runtime.machine().spu();
  const auto &spu_state = spu.state();
  const auto active_voices = static_cast<std::size_t>(
      std::ranges::count_if(spu_state.voices, [](const auto &voice) noexcept {
        return voice.active != 0U;
      }));
  const auto old_flags = std::cout.flags();
  const auto old_fill = std::cout.fill();
  std::cout << "snapshot frame=" << frame << " stage=" << stageName(stage)
            << " pc=0x" << std::hex << std::setw(8) << std::setfill('0')
            << cpu.pc << " epc=0x" << std::setw(8) << cpu.cop0_epc << " ra=0x"
            << std::setw(8) << cpu.gpr[31U] << " sp=0x" << std::setw(8)
            << cpu.gpr[29U] << " pad=" << std::setw(4) << pad.buttons
            << " pad_id=0x" << std::setw(2) << static_cast<unsigned>(pad.id)
            << std::dec << " status=0x" << std::hex << cpu.cop0_status
            << " cause=0x" << cpu.cop0_cause << " istat=0x" << interrupt_status
            << " imask=0x" << interrupt_mask << " irq_entry=0x"
            << runtime.biosState().interrupt_entry << std::dec
            << " gp0_total=" << stats.gpu_words
            << " gp0_frame=" << runtime.gpuCommands().size()
            << " display=" << display.width << 'x' << display.height
            << " enabled=" << display.enabled << " cd_lba=" << cd.current_lba
            << " cd_sectors=" << cd.sectors_read << " cd_reading=" << cd.reading
            << " audio_frames=" << audio_frames << " spu_control=0x" << std::hex
            << spu.control() << " spu_status=0x" << spu.status() << std::dec
            << " spu_mixed=" << spu_state.mixed_frames
            << " spu_pcm=" << spu.queuedPcmFrames()
            << " spu_dropped=" << spu.droppedPcmFrames()
            << " spu_cd=" << spu.queuedCdFrames()
            << " spu_active=" << active_voices << '\n';
  for (std::size_t voice{}; voice < spu_state.voices.size(); ++voice) {
    const auto &state = spu_state.voices[voice];
    if (state.active != 0U) {
      std::cout << " spu_voice" << voice << "=0x" << std::hex
                << state.block_address << "/0x" << state.repeat_address
                << std::dec << ':' << static_cast<unsigned>(state.adsr_phase);
    }
  }
  std::cout << '\n';
  for (std::size_t priority{};
       priority < runtime.biosState().interrupt_routines.size(); ++priority) {
    std::cout << " irq_head" << priority << "=0x" << std::hex
              << runtime.biosState().interrupt_routines[priority];
  }
  std::cout << std::dec << '\n';
  std::cout.flags(old_flags);
  std::cout.fill(old_fill);
}

[[noreturn]] void failAt(const mohu::Runtime &runtime, std::uint64_t frame,
                         ProbeStage stage, std::string_view detail) {
  std::cerr << "MOHU gameplay probe failed at frame " << frame
            << " stage=" << stageName(stage) << ": " << detail << '\n';
  printSnapshot(runtime, frame, stage, 0U);
  throw std::runtime_error{std::string{detail}};
}

#if defined(MOHU_GAMEPLAY_RENDER_PROBE)
int replayMapCheckpoint(const std::filesystem::path &path,
                        RenderProbeMode mode) {
  MapCheckpointReader reader{path};
  RenderProbeSession renderer;
  renderer.start(mode);

  MapReplayFrame frame;
  std::uint64_t replayed_frames{};
  std::uint64_t replayed_gameplay_frames{};
  std::uint64_t observed_first_source_frame{};
  std::uint64_t observed_first_gameplay_frame{};
  std::uint64_t polygons_before_gameplay{};
  while (reader.readFrame(frame)) {
    if (replayed_frames == 0U) {
      observed_first_source_frame = frame.source_frame;
    }
    if (frame.stage == ProbeStage::gameplay && replayed_gameplay_frames == 0U) {
      observed_first_gameplay_frame = frame.source_frame;
      polygons_before_gameplay = renderer.polygonPrimitives();
    }
    const auto projections =
        mode == RenderProbeMode::baseline_no_pgxp
            ? std::span<const sf::psx::GteProjectedVertex>{}
            : std::span<const sf::psx::GteProjectedVertex>{frame.projections};
    const auto identities =
        mode == RenderProbeMode::baseline_no_pgxp
            ? std::span<const std::uint64_t>{}
            : std::span<const std::uint64_t>{frame.projection_identities};
    const auto gameplay = frame.stage == ProbeStage::gameplay;
    renderer.render(frame.words, projections, identities, {}, frame.display,
                    frame.command_buffer_epoch, false, gameplay, gameplay);
    ++replayed_frames;
    replayed_gameplay_frames += frame.stage == ProbeStage::gameplay ? 1U : 0U;
  }
  reader.validateEnd();
  const auto gameplay_polygons =
      renderer.polygonPrimitives() - polygons_before_gameplay;
  if (replayed_frames != reader.frameCount() ||
      observed_first_source_frame != reader.firstSourceFrame() ||
      replayed_gameplay_frames != reader.gameplayFrameCount() ||
      observed_first_gameplay_frame != reader.firstGameplayFrame() ||
      gameplay_polygons == 0U) {
    throw std::runtime_error{
        "Map checkpoint replay did not reach gameplay geometry"};
  }

  renderer.captureCurrentFrame();
  std::cout << "MOHU direct map replay passed: checkpoint=" << path.string()
            << " frames=" << replayed_frames
            << " source_first=" << reader.firstSourceFrame()
            << " gameplay_first=" << observed_first_gameplay_frame
            << " gameplay_frames=" << replayed_gameplay_frames
            << " gameplay_polygons=" << gameplay_polygons << '\n';
  renderer.printSummary(mode);
  return 0;
}
#endif

} // namespace

int main(int argc, char **argv) {
  try {
    const auto print_usage = [] {
      std::cerr << "Usage:\n"
                   "  mohu_gameplay_probe <game.bin|game.cue> [maximum-frames] "
                   "[required-gameplay-frames] [render-mode] "
                   "[--record-map <checkpoint>]\n"
                   "  mohu_gameplay_probe --replay-map <checkpoint> "
                   "[render-mode]\n"
                   "Render modes: --render-baseline, --render-no-depth, "
                   "--render-no-pgxp, --render-exact-cpu, --render-quality\n";
    };
    if (argc >= 2 && std::string_view{argv[1]} == "--replay-map") {
#if defined(MOHU_GAMEPLAY_RENDER_PROBE)
      if (argc < 3 || argc > 4) {
        print_usage();
        return 64;
      }
      auto render_mode = RenderProbeMode::baseline;
      if (argc == 4) {
        const auto parsed = parseRenderProbeMode(argv[3]);
        if (!parsed) {
          throw std::invalid_argument{"Unknown render probe mode"};
        }
        render_mode = *parsed;
      }
      return replayMapCheckpoint(std::filesystem::path{argv[2]}, render_mode);
#else
      throw std::invalid_argument{
          "This build has no PsyCross map replay support"};
#endif
    }
    if (argc < 2) {
      print_usage();
      return 64;
    }

    auto argument = 2;
    auto maximum_frames = default_maximum_frames;
    auto required_gameplay_frames = default_gameplay_frames;
    if (argument < argc &&
        !std::string_view{argv[argument]}.starts_with("--")) {
      maximum_frames = std::stoull(argv[argument++]);
    }
    if (argument < argc &&
        !std::string_view{argv[argument]}.starts_with("--")) {
      required_gameplay_frames = std::stoull(argv[argument++]);
    }

    std::optional<std::filesystem::path> record_map_path;
#if defined(MOHU_GAMEPLAY_RENDER_PROBE)
    auto render_mode = RenderProbeMode::disabled;
#endif
    while (argument < argc) {
      const std::string_view option{argv[argument++]};
#if defined(MOHU_GAMEPLAY_RENDER_PROBE)
      if (const auto parsed = parseRenderProbeMode(option)) {
        if (render_mode != RenderProbeMode::disabled) {
          throw std::invalid_argument{"Duplicate render probe mode"};
        }
        render_mode = *parsed;
        continue;
      }
#endif
      if (option == "--record-map") {
        if (record_map_path || argument >= argc) {
          throw std::invalid_argument{"Invalid --record-map option"};
        }
        record_map_path = std::filesystem::path{argv[argument++]};
        if (record_map_path->empty()) {
          throw std::invalid_argument{"Empty map checkpoint path"};
        }
        continue;
      }
      throw std::invalid_argument{"Unknown gameplay probe option"};
    }
#if defined(MOHU_GAMEPLAY_RENDER_PROBE)
    RenderProbeSession renderer;
#endif
    if (required_gameplay_frames == 0U ||
        maximum_frames < menu_pulse_start_frame ||
        required_gameplay_frames > maximum_frames - menu_pulse_start_frame) {
      throw std::invalid_argument{"Frame limit is too short for gameplay"};
    }

    auto disc = sf::game::GameDisc::open(std::filesystem::path{argv[1]});
    std::vector<DiscExtent> mission_extents;
    for (const auto path : {
             std::string_view{"DATA/MSN2/LVL1/2_1.BSD"},
             std::string_view{"DATA/MSN2/LVL1/2_10.TAF"},
             std::string_view{"DATA/MSN2/LVL1/M2_1_1.VB"},
         }) {
      mission_extents.push_back(findExtent(disc, path));
    }
    for (const auto &extent : mission_extents) {
      std::cout << "target path=" << extent.path << " lba=" << extent.first_lba
                << " sectors=" << extent.sector_count << '\n';
    }

    auto runtime = mohu::Runtime{std::move(disc)};
    auto capture_projection_identities = record_map_path.has_value();
#if defined(MOHU_GAMEPLAY_RENDER_PROBE)
    capture_projection_identities =
        capture_projection_identities ||
        (render_mode != RenderProbeMode::disabled &&
         render_mode != RenderProbeMode::baseline_no_pgxp);
#endif
    runtime.setGpuProjectionIdentityTracking(capture_projection_identities);
    auto &probe_cpu = const_cast<sf::psx::R3000Runtime &>(runtime.cpu());
    static_cast<void>(probe_cpu.setPgxpTransformTracking(false));
    std::optional<MapCheckpointWriter> map_checkpoint;
    if (record_map_path) {
      map_checkpoint.emplace(*record_map_path);
    }
    auto stage = ProbeStage::boot;
    std::uint64_t first_mission_read_frame{};
    std::uint64_t gameplay_start_frame{};
    std::uint64_t last_gpu_progress_frame{};
    std::uint64_t previous_gpu_words{};
    std::uint64_t audio_frames{};
    std::uint64_t projected_gpu_words{};
    std::uint64_t frames_with_projected_gpu_words{};
    std::uint64_t identified_gpu_words{};
    std::uint64_t frames_with_identified_gpu_words{};
    PgxpPrimitiveCoverage pgxp_coverage{};
    bool saw_analog_pad{};
    bool saw_gameplay_forward{};
    bool saw_gameplay_fire{};
    std::string matched_mission_path;
    std::array<sf::psx::SpuPcmFrame, 4'096U> audio_scratch{};

    for (std::uint64_t frame = 1U; frame <= maximum_frames; ++frame) {
      bool enable_pgxp_tracking = true;
#if defined(MOHU_GAMEPLAY_RENDER_PROBE)
      enable_pgxp_tracking = render_mode != RenderProbeMode::baseline_no_pgxp;
#endif
      if (frame == menu_pulse_start_frame && enable_pgxp_tracking &&
          !probe_cpu.setPgxpTransformTracking(true)) {
        throw std::runtime_error{"Cannot enable PGXP coverage tracking"};
      }
#if defined(MOHU_GAMEPLAY_RENDER_PROBE)
      if (frame == menu_pulse_start_frame &&
          render_mode == RenderProbeMode::baseline_exact_cpu) {
        probe_cpu.setPgxpCpuTracking(true);
      }
#endif
#if defined(MOHU_GAMEPLAY_RENDER_PROBE)
      if (frame == menu_pulse_start_frame) {
        renderer.start(render_mode);
      }
#endif
      if (frame >= menu_pulse_start_frame && stage == ProbeStage::boot) {
        stage = ProbeStage::menu_navigation;
        std::cout << "transition frame=" << frame << " stage=menu\n";
      }

      const auto gameplay_frame =
          gameplay_start_frame == 0U ? 0U : frame - gameplay_start_frame;
      const auto requested_buttons =
          scriptedButtons(frame, stage, gameplay_frame);
      const auto result = runtime.runFrame(requested_buttons, centered_axes);
      if (!result.running()) {
        std::cerr << "MOHU gameplay probe guest stop frame=" << frame
                  << " stage=" << stageName(stage)
                  << " detail=" << result.detail << '\n';
        printSnapshot(runtime, frame, stage, audio_frames);
        const auto old_flags = std::cout.flags();
        const auto old_fill = std::cout.fill();
        std::cout << "fault_code";
        for (std::uint32_t address = 0x80025320U; address < 0x800253c0U;
             address += 4U) {
          std::uint32_t word{};
          if (runtime.cpu().read32(address, word)) {
            std::cout << " [0x" << std::hex << address << "]=0x" << std::setw(8)
                      << std::setfill('0') << word;
          }
        }
        std::cout << std::dec << '\n';
        std::cout.flags(old_flags);
        std::cout.fill(old_fill);
        return 3;
      }
#if defined(MOHU_GAMEPLAY_RENDER_PROBE)
      renderer.render(runtime, stage == ProbeStage::gameplay &&
                                   gameplay_frame >= required_gameplay_frames);
#endif
      for (;;) {
        const auto drained = runtime.takePcm(audio_scratch);
        audio_frames += drained;
        if (drained < audio_scratch.size()) {
          break;
        }
      }
      const auto frame_projected_words =
          static_cast<std::uint64_t>(std::ranges::count_if(
              runtime.gpuProjections(),
              [](const auto &vertex) { return vertex.valid; }));
      projected_gpu_words += frame_projected_words;
      frames_with_projected_gpu_words += frame_projected_words != 0U ? 1U : 0U;
      const auto frame_identified_words =
          static_cast<std::uint64_t>(std::ranges::count_if(
              runtime.gpuProjectionIdentities(),
              [](std::uint64_t identity) { return identity != 0U; }));
      identified_gpu_words += frame_identified_words;
      frames_with_identified_gpu_words +=
          frame_identified_words != 0U ? 1U : 0U;
      if (stage == ProbeStage::gameplay) {
        accumulatePgxpCoverage(runtime.gpuCommands(), runtime.gpuProjections(),
                               pgxp_coverage);
      }

      const auto pad = readPad(runtime);
      const auto level_buttons = readLevelButtons(runtime);
      saw_analog_pad = saw_analog_pad || isLiveLevelPad(pad);
      if (stage == ProbeStage::mission_loading &&
          requested_buttons == cross_buttons && isLiveLevelPad(pad) &&
          pad.buttons == requested_buttons) {
        stage = ProbeStage::gameplay;
        gameplay_start_frame = frame;
        std::cout << "transition frame=" << frame
                  << " stage=gameplay level-pad-confirmed id=0x" << std::hex
                  << static_cast<unsigned>(pad.id) << " buttons=0x"
                  << pad.buttons << std::dec << '\n';
        printSnapshot(runtime, frame, stage, audio_frames);
      }
      if (stage == ProbeStage::gameplay && isLiveLevelPad(pad)) {
        saw_gameplay_forward = saw_gameplay_forward ||
                               ((pad.buttons & dpad_up_mask) == 0U &&
                                (level_buttons & level_dpad_up_mask) != 0U);
        saw_gameplay_fire =
            saw_gameplay_fire || ((pad.buttons & circle_mask) == 0U &&
                                  (level_buttons & level_circle_mask) != 0U);
      }

      const auto cd = runtime.machine().cdrom().captureState();
      std::string_view current_match;
      if (recentReadsContain(cd, mission_extents, current_match)) {
        matched_mission_path = current_match;
        if (first_mission_read_frame == 0U) {
          first_mission_read_frame = frame;
          stage = ProbeStage::mission_loading;
          std::cout << "transition frame=" << frame
                    << " stage=mission-load path=" << matched_mission_path
                    << " lba=" << cd.current_lba << '\n';
          printSnapshot(runtime, frame, stage, audio_frames);
        }
      }

      if (map_checkpoint) {
        map_checkpoint->append(runtime, frame, stage);
      }

      const auto gpu_words = runtime.stats().gpu_words;
      if (gpu_words != previous_gpu_words) {
        previous_gpu_words = gpu_words;
        last_gpu_progress_frame = frame;
      }
      if (stage == ProbeStage::gameplay &&
          frame - last_gpu_progress_frame >= gameplay_stall_frames) {
        failAt(runtime, frame, stage,
               "no guest GPU progress for 900 consecutive frames");
      }

      if (frame == 1U || frame % snapshot_period_frames == 0U) {
        printSnapshot(runtime, frame, stage, audio_frames);
      }

      if (stage == ProbeStage::gameplay && saw_gameplay_forward &&
          saw_gameplay_fire &&
          frame >= gameplay_start_frame + required_gameplay_frames) {
        printSnapshot(runtime, frame, stage, audio_frames);
        if (map_checkpoint) {
          map_checkpoint->finalize();
          std::cout << "MOHU map checkpoint recorded: path="
                    << map_checkpoint->path().string()
                    << " frames=" << map_checkpoint->frameCount()
                    << " gameplay_frames="
                    << map_checkpoint->gameplayFrameCount() << '\n';
        }
        std::cout << "MOHU gameplay probe passed: mission_path="
                  << matched_mission_path
                  << " mission_read_frame=" << first_mission_read_frame
                  << " gameplay_frame=" << gameplay_start_frame
                  << " survived_frames=" << frame - gameplay_start_frame
                  << " total_frames=" << frame
                  << " instructions=" << runtime.stats().instructions
                  << " gpu_words=" << runtime.stats().gpu_words
                  << " projected_gpu_words=" << projected_gpu_words
                  << " projected_gpu_frames=" << frames_with_projected_gpu_words
                  << " identified_gpu_words=" << identified_gpu_words
                  << " identified_gpu_frames="
                  << frames_with_identified_gpu_words
                  << " polygon_primitives=" << pgxp_coverage.polygons
                  << " precise_polygon_primitives="
                  << pgxp_coverage.precise_polygons
                  << " partial_polygon_primitives="
                  << pgxp_coverage.partial_polygons
                  << " exact_transform_polygon_primitives="
                  << pgxp_coverage.exact_transform_polygons
                  << " fractional_transform_polygon_primitives="
                  << pgxp_coverage.fractional_transform_polygons
                  << " exact_near_plane_polygon_primitives="
                  << pgxp_coverage.exact_near_plane_polygons
                  << " audio_frames=" << audio_frames << '\n';
#if defined(MOHU_GAMEPLAY_RENDER_PROBE)
        renderer.printSummary(render_mode);
#endif
        return 0;
      }
    }

    if (!saw_analog_pad) {
      failAt(runtime, maximum_frames, stage,
             "LEVEL runtime never exposed a live digital/analog pad");
    }
    if (first_mission_read_frame == 0U) {
      failAt(runtime, maximum_frames, stage,
             "script did not reach the first mission data");
    }
    failAt(runtime, maximum_frames, stage,
           "first mission did not survive the gameplay exercise window");
  } catch (const std::exception &error) {
    std::cerr << "mohu_gameplay_probe: " << error.what() << '\n';
    return 1;
  }
}
