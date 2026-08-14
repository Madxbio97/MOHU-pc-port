#pragma once

#include "mohu/runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <system_error>

extern "C" void PsyX_Log_Info(const char *format, ...);

namespace mohu::app {

[[nodiscard]] inline std::filesystem::path
configureRendererDiagnosticLog(const std::filesystem::path &memory_card_path) {
  const auto path =
      memory_card_path.parent_path().parent_path() / L"mohu-render.log";
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (!error) {
#ifdef _WIN32
    _putenv_s("PSYX_LOG_FILE", path.string().c_str());
#else
    setenv("PSYX_LOG_FILE", path.string().c_str(), 1);
#endif
  }
  return path;
}
inline void
accumulateRenderStats(NativeRenderFrameStats &total,
                      const NativeRenderFrameStats &frame) noexcept {
  total.commands += frame.commands;
  total.state_commands += frame.state_commands;
  total.scene_polygons += frame.scene_polygons;
  total.exact_view_polygons += frame.exact_view_polygons;
  total.exact_view_vertices += frame.exact_view_vertices;
  total.identified_exact_vertices += frame.identified_exact_vertices;
  total.unique_scene_positions += frame.unique_scene_positions;
  total.shared_scene_vertices += frame.shared_scene_vertices;
  total.conflicting_scene_vertices += frame.conflicting_scene_vertices;
  total.projected_polygons += frame.projected_polygons;
  total.legacy_polygons += frame.legacy_polygons;
  total.screen_primitives += frame.screen_primitives;
  total.vram_commands += frame.vram_commands;
  total.unknown_commands += frame.unknown_commands;
  total.truncated_words += frame.truncated_words;
  total.build_failures += frame.build_failures;
  total.hybrid_seam_edges += frame.hybrid_seam_edges;
  total.hybrid_seam_positions += frame.hybrid_seam_positions;
  total.hybrid_seam_rejections += frame.hybrid_seam_rejections;
}

class RendererDiagnostics final {
public:
  using Clock = std::chrono::steady_clock;

  explicit RendererDiagnostics(const RuntimeStats &initial) noexcept
      : previous_(initial), period_start_(Clock::now()) {}

  [[nodiscard]] Clock::time_point beginFrame() const noexcept {
    return Clock::now();
  }

  void finishFrame(
      const RuntimeStats &current, const NativeRenderFrameStats &render_frame,
      std::span<const sf::psx::R3000StoreSiteDiagnostic> store_sites,
      std::uint64_t store_overflow, Clock::time_point frame_start) noexcept {
    accumulateRenderStats(render_stats_, render_frame);
    const auto now = Clock::now();
    runtime_nanoseconds_ += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - frame_start)
            .count());
    const auto elapsed = std::chrono::duration<double>(now - period_start_);
    if (elapsed.count() < 5.0) {
      return;
    }

    const auto frames = current.frames - previous_.frames;
    PsyX_Log_Info(
        "[RuntimeDiag] guest_frames=%llu guest_hz=%.2f "
        "cpu_avg=%.3fms instructions_avg=%.0f gpu_words_avg=%.1f "
        "bios=%llu interrupts=%llu\n",
        static_cast<unsigned long long>(frames),
        elapsed.count() > 0.0 ? static_cast<double>(frames) / elapsed.count()
                              : 0.0,
        frames == 0U ? 0.0
                     : static_cast<double>(runtime_nanoseconds_) /
                           static_cast<double>(frames) / 1'000'000.0,
        frames == 0U ? 0.0
                     : static_cast<double>(current.instructions -
                                           previous_.instructions) /
                           static_cast<double>(frames),
        frames == 0U
            ? 0.0
            : static_cast<double>(current.gpu_words - previous_.gpu_words) /
                  static_cast<double>(frames),
        static_cast<unsigned long long>(current.bios_calls -
                                        previous_.bios_calls),
        static_cast<unsigned long long>(current.interrupts -
                                        previous_.interrupts));

    const auto native_coverage =
        render_stats_.scene_polygons == 0U
            ? 100.0
            : static_cast<double>(render_stats_.exact_view_polygons) * 100.0 /
                  static_cast<double>(render_stats_.scene_polygons);
    PsyX_Log_Info(
        "[RenderContract] commands=%llu scene=%llu exact=%llu "
        "coverage=%.2f%% identified=%llu/%llu topology=%llu/%llu "
        "conflicts=%llu seams=%llu/%llu/%llu "
        "projected=%llu legacy=%llu screen=%llu vram=%llu unknown=%llu "
        "truncated=%llu failures=%llu\n",
        static_cast<unsigned long long>(render_stats_.commands),
        static_cast<unsigned long long>(render_stats_.scene_polygons),
        static_cast<unsigned long long>(render_stats_.exact_view_polygons),
        native_coverage,
        static_cast<unsigned long long>(
            render_stats_.identified_exact_vertices),
        static_cast<unsigned long long>(render_stats_.exact_view_vertices),
        static_cast<unsigned long long>(render_stats_.unique_scene_positions),
        static_cast<unsigned long long>(render_stats_.shared_scene_vertices),
        static_cast<unsigned long long>(
            render_stats_.conflicting_scene_vertices),
        static_cast<unsigned long long>(render_stats_.hybrid_seam_edges),
        static_cast<unsigned long long>(render_stats_.hybrid_seam_positions),
        static_cast<unsigned long long>(render_stats_.hybrid_seam_rejections),
        static_cast<unsigned long long>(render_stats_.projected_polygons),
        static_cast<unsigned long long>(render_stats_.legacy_polygons),
        static_cast<unsigned long long>(render_stats_.screen_primitives),
        static_cast<unsigned long long>(render_stats_.vram_commands),
        static_cast<unsigned long long>(render_stats_.unknown_commands),
        static_cast<unsigned long long>(render_stats_.truncated_words),
        static_cast<unsigned long long>(render_stats_.build_failures));

    std::array<sf::psx::R3000StoreSiteDiagnostic,
               sf::psx::R3000Runtime::store_diagnostic_capacity>
        sorted_sites{};
    std::copy_n(store_sites.begin(),
                std::min(store_sites.size(), sorted_sites.size()),
                sorted_sites.begin());
    std::partial_sort(sorted_sites.begin(), sorted_sites.begin() + 6U,
                      sorted_sites.end(),
                      [](const auto &first, const auto &second) {
                        return first.writes > second.writes;
                      });
    PsyX_Log_Info(
        "[RuntimeDiag][stores] overflow=%llu "
        "%08x:%llu/%llu[h%llu u%llu c%llu] "
        "%08x:%llu/%llu %08x:%llu/%llu %08x:%llu/%llu "
        "%08x:%llu/%llu %08x:%llu/%llu\n",
        static_cast<unsigned long long>(store_overflow), sorted_sites[0U].pc,
        static_cast<unsigned long long>(sorted_sites[0U].writes),
        static_cast<unsigned long long>(sorted_sites[0U].projected_writes),
        static_cast<unsigned long long>(sorted_sites[0U].halfword_writes),
        static_cast<unsigned long long>(sorted_sites[0U].unaligned_writes),
        static_cast<unsigned long long>(sorted_sites[0U].cop2_writes),
        sorted_sites[1U].pc,
        static_cast<unsigned long long>(sorted_sites[1U].writes),
        static_cast<unsigned long long>(sorted_sites[1U].projected_writes),
        sorted_sites[2U].pc,
        static_cast<unsigned long long>(sorted_sites[2U].writes),
        static_cast<unsigned long long>(sorted_sites[2U].projected_writes),
        sorted_sites[3U].pc,
        static_cast<unsigned long long>(sorted_sites[3U].writes),
        static_cast<unsigned long long>(sorted_sites[3U].projected_writes),
        sorted_sites[4U].pc,
        static_cast<unsigned long long>(sorted_sites[4U].writes),
        static_cast<unsigned long long>(sorted_sites[4U].projected_writes),
        sorted_sites[5U].pc,
        static_cast<unsigned long long>(sorted_sites[5U].writes),
        static_cast<unsigned long long>(sorted_sites[5U].projected_writes));
    previous_ = current;
    period_start_ = now;
    runtime_nanoseconds_ = 0U;
    render_stats_ = {};
  }

  static void announce(const std::filesystem::path &path) noexcept {
    PsyX_Log_Info("[RuntimeDiag] log=%s\n", path.string().c_str());
  }

private:
  RuntimeStats previous_{};
  Clock::time_point period_start_{};
  std::uint64_t runtime_nanoseconds_{};
  NativeRenderFrameStats render_stats_{};
};

} // namespace mohu::app
