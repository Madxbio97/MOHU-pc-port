#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace sf::platform {

// Computes the callback sink's recovery high-water mark independently from
// OpenAL so device-period and producer-jitter behavior remains testable.
[[nodiscard]] inline std::size_t
audioCallbackPredictedRequestFrames(std::uint32_t sample_rate,
                                    std::uint32_t device_refresh_hz,
                                    std::size_t ring_capacity) noexcept {
  if (sample_rate == 0U || device_refresh_hz == 0U || ring_capacity == 0U) {
    return 0U;
  }
  const auto device_frames =
      (static_cast<std::uint64_t>(sample_rate) + device_refresh_hz - 1U) /
      device_refresh_hz;
  return static_cast<std::size_t>(
      std::min<std::uint64_t>(device_frames, ring_capacity));
}

[[nodiscard]] inline std::size_t audioCallbackStartFrames(
    std::size_t jitter_reserve_frames, std::uint32_t sample_rate,
    std::uint32_t device_refresh_hz, std::size_t ring_capacity) noexcept {
  if (ring_capacity == 0U) {
    return 0U;
  }
  const auto reserve =
      std::clamp<std::size_t>(jitter_reserve_frames, 1U, ring_capacity);
  const auto predicted_request = audioCallbackPredictedRequestFrames(
      sample_rate, device_refresh_hz, ring_capacity);
  if (predicted_request == 0U) {
    return reserve;
  }
  if (predicted_request >= ring_capacity - reserve) {
    return ring_capacity;
  }
  // Initial playback keeps one predicted device request plus a small cushion.
  return predicted_request + reserve;
}

// Starvation recovery is intentionally deeper than initial startup. Runtime
// publishes all PCM from one host iteration atomically after guest_steps, so
// recovery must leave a whole producer batch after satisfying the callback.
[[nodiscard]] inline std::size_t
audioCallbackRecoveryFrames(std::size_t base_start_frames,
                            std::size_t callback_request_frames,
                            std::size_t maximum_producer_batch_frames,
                            std::size_t ring_capacity) noexcept {
  if (ring_capacity == 0U) {
    return 0U;
  }
  const auto base = std::min(base_start_frames, ring_capacity);
  const auto request = std::min(callback_request_frames, ring_capacity);
  const auto producer = std::min(maximum_producer_batch_frames, ring_capacity);
  const auto request_with_reserve =
      request >= ring_capacity - producer ? ring_capacity : request + producer;
  return std::max(base, request_with_reserve);
}

// A reset clears buffered PCM but preserves the largest callback request seen.
// Do not restart the source until that learned request can be served in full.
[[nodiscard]] inline bool
audioCallbackStartupReady(std::size_t available_frames,
                          std::size_t base_start_frames,
                          std::size_t learned_request_frames) noexcept {
  return available_frames >=
         std::max(base_start_frames, learned_request_frames);
}

[[nodiscard]] inline bool
audioCallbackRecoveryReady(bool recovering, std::size_t available_frames,
                           std::size_t start_frames) noexcept {
  return !recovering || available_frames >= start_frames;
}

enum class AudioTimelineCapacityAction : std::uint8_t {
  accept,
  rebuffer,
  reject,
};

// A continuous realtime stream cannot usefully play PCM which is already an
// entire FIFO behind the guest clock. Recover by dropping that stale sink
// generation and prebuffering the current producer block. One-shot streams
// remain strict because silently truncating a cue would change its content.
[[nodiscard]] inline AudioTimelineCapacityAction
audioTimelineCapacityAction(std::size_t staged_frames,
                            std::size_t incoming_frames, std::size_t capacity,
                            bool continuous) noexcept {
  if (incoming_frames > capacity) {
    return AudioTimelineCapacityAction::reject;
  }
  const auto occupied = std::min(staged_frames, capacity);
  if (incoming_frames <= capacity - occupied) {
    return AudioTimelineCapacityAction::accept;
  }
  return continuous ? AudioTimelineCapacityAction::rebuffer
                    : AudioTimelineCapacityAction::reject;
}

// The periodic sink diagnostics retain the exact cumulative counter, so the
// realtime warning only needs the first event and one checkpoint per bucket.
[[nodiscard]] inline bool
audioStarvationDiagnosticDue(std::uint64_t previously_observed,
                             std::uint64_t current,
                             std::uint64_t interval = 64U) noexcept {
  if (current <= previously_observed || interval == 0U) {
    return false;
  }
  return previously_observed == 0U ||
         current / interval != previously_observed / interval;
}

// Single-producer/single-consumer PCM ring used between the emulation thread
// and OpenAL Soft's realtime callback. The storage is allocated once; neither
// push nor pop allocates, blocks, or takes a lock. A short producer stall can
// therefore become an exact interval of silence without stopping/restarting
// the device source or replaying stale buffers afterwards.
template <typename Frame> class AudioFrameRing final {
public:
  explicit AudioFrameRing(std::size_t capacity)
      : frames_(std::max<std::size_t>(capacity, 1U)) {}

  [[nodiscard]] std::size_t push(std::span<const Frame> source) noexcept {
    const auto write = write_position_.load(std::memory_order_relaxed);
    const auto read = read_position_.load(std::memory_order_acquire);
    const auto occupied = static_cast<std::size_t>(write - read);
    const auto writable = frames_.size() - std::min(occupied, frames_.size());
    const auto count = std::min(source.size(), writable);
    if (count == 0U) {
      return 0U;
    }
    const auto offset = static_cast<std::size_t>(write % frames_.size());
    const auto first = std::min(count, frames_.size() - offset);
    std::copy_n(source.begin(), first,
                frames_.begin() + static_cast<std::ptrdiff_t>(offset));
    std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(first),
                count - first, frames_.begin());
    write_position_.store(write + count, std::memory_order_release);
    return count;
  }

  [[nodiscard]] std::size_t pop(std::span<Frame> destination) noexcept {
    const auto read = read_position_.load(std::memory_order_relaxed);
    const auto write = write_position_.load(std::memory_order_acquire);
    const auto readable = static_cast<std::size_t>(write - read);
    const auto count = std::min(destination.size(), readable);
    if (count == 0U) {
      return 0U;
    }
    const auto offset = static_cast<std::size_t>(read % frames_.size());
    const auto first = std::min(count, frames_.size() - offset);
    std::copy_n(frames_.begin() + static_cast<std::ptrdiff_t>(offset), first,
                destination.begin());
    std::copy_n(frames_.begin(), count - first,
                destination.begin() + static_cast<std::ptrdiff_t>(first));
    read_position_.store(read + count, std::memory_order_release);
    return count;
  }

  void clear() noexcept {
    read_position_.store(write_position_.load(std::memory_order_acquire),
                         std::memory_order_release);
  }

  [[nodiscard]] std::size_t size() const noexcept {
    const auto write = write_position_.load(std::memory_order_acquire);
    const auto read = read_position_.load(std::memory_order_acquire);
    return std::min(static_cast<std::size_t>(write - read), frames_.size());
  }
  [[nodiscard]] std::size_t capacity() const noexcept { return frames_.size(); }

private:
  std::vector<Frame> frames_;
  std::atomic<std::uint64_t> read_position_{};
  std::atomic<std::uint64_t> write_position_{};
};

template <typename Frame> class AudioTempoStretcher final {
public:
  void reset() noexcept {
    input_.clear();
    tail_.fill({});
    nominal_position_ = 0.0;
    actual_position_ = 0U;
    initialized_ = false;
  }

  void process(std::span<const Frame> source, double tempo,
               std::vector<Frame> &output) {
    if (source.empty()) {
      return;
    }
    if (!std::isfinite(tempo)) {
      tempo = 1.0;
    }
    tempo = std::clamp(tempo, 0.5, 1.0);
    input_.insert(input_.end(), source.begin(), source.end());
    output.reserve(output.size() + source.size() * 2U + overlap_frames);

    if (!initialized_) {
      if (input_.size() < window_frames) {
        return;
      }
      output.insert(output.end(), input_.begin(),
                    input_.begin() + overlap_frames);
      std::copy_n(input_.begin() + overlap_frames, overlap_frames,
                  tail_.begin());
      initialized_ = true;
    }

    for (;;) {
      const auto next_nominal =
          nominal_position_ + static_cast<double>(overlap_frames) * tempo;
      if (input_.size() < window_frames) {
        break;
      }
      const auto maximum_candidate = input_.size() - window_frames;
      const auto center = static_cast<std::size_t>(
          std::max(0.0, std::floor(next_nominal + 0.5)));
      const auto begin = std::max(
          actual_position_ + 1U,
          center > search_frames ? center - search_frames : std::size_t{});
      const auto end = std::min(maximum_candidate, center + search_frames);
      if (begin > end) {
        break;
      }

      const auto candidate = std::abs(tempo - 1.0) < 1.0e-9
                                 ? std::clamp(center, begin, end)
                                 : bestCandidate(begin, end, next_nominal);
      for (std::size_t index{}; index < overlap_frames; ++index) {
        auto mixed = tail_[index];
        const auto &incoming = input_[candidate + index];
        mixed.left = blend(tail_[index].left, incoming.left, index);
        mixed.right = blend(tail_[index].right, incoming.right, index);
        output.push_back(mixed);
      }
      std::copy_n(input_.begin() +
                      static_cast<std::ptrdiff_t>(candidate + overlap_frames),
                  overlap_frames, tail_.begin());
      nominal_position_ = next_nominal;
      actual_position_ = candidate;
    }
    compact();
  }

private:
  static constexpr std::size_t window_frames = 512U;
  static constexpr std::size_t overlap_frames = window_frames / 2U;
  static constexpr std::size_t search_frames = 64U;
  static constexpr std::size_t correlation_stride = 4U;

  template <typename Sample>
  [[nodiscard]] static Sample blend(Sample previous, Sample incoming,
                                    std::size_t position) noexcept {
    const auto numerator =
        static_cast<std::int64_t>(previous) *
            static_cast<std::int64_t>(overlap_frames - position) +
        static_cast<std::int64_t>(incoming) *
            static_cast<std::int64_t>(position);
    return static_cast<Sample>(numerator /
                               static_cast<std::int64_t>(overlap_frames));
  }

  [[nodiscard]] std::size_t bestCandidate(std::size_t begin, std::size_t end,
                                          double nominal) const noexcept {
    auto best = begin;
    auto best_score = -std::numeric_limits<double>::infinity();
    for (auto candidate = begin; candidate <= end; ++candidate) {
      std::int64_t dot{};
      std::uint64_t tail_energy{};
      std::uint64_t candidate_energy{};
      for (std::size_t index{}; index < overlap_frames;
           index += correlation_stride) {
        const auto accumulate = [&](auto previous, auto incoming) {
          const auto a = static_cast<std::int64_t>(previous);
          const auto b = static_cast<std::int64_t>(incoming);
          dot += a * b;
          tail_energy += static_cast<std::uint64_t>(a * a);
          candidate_energy += static_cast<std::uint64_t>(b * b);
        };
        accumulate(tail_[index].left, input_[candidate + index].left);
        accumulate(tail_[index].right, input_[candidate + index].right);
      }
      auto score = -std::abs(static_cast<double>(candidate) - nominal) * 1.0e-6;
      if (tail_energy != 0U && candidate_energy != 0U) {
        score += static_cast<double>(dot) /
                 std::sqrt(static_cast<double>(tail_energy) *
                           static_cast<double>(candidate_energy));
      }
      if (score > best_score) {
        best_score = score;
        best = candidate;
      }
    }
    return best;
  }

  void compact() {
    if (actual_position_ < 4U * window_frames) {
      return;
    }
    const auto nominal = nominal_position_ > 0.0
                             ? static_cast<std::size_t>(nominal_position_)
                             : std::size_t{};
    const auto count = std::min(actual_position_, nominal);
    input_.erase(input_.begin(),
                 input_.begin() + static_cast<std::ptrdiff_t>(count));
    actual_position_ -= count;
    nominal_position_ -= static_cast<double>(count);
  }

  std::vector<Frame> input_;
  std::array<Frame, overlap_frames> tail_{};
  double nominal_position_{};
  std::size_t actual_position_{};
  bool initialized_{};
};

// Pure, deterministic start/recovery policy used by the OpenAL sink. Initial
// playback waits for a small prebuffer. Recovery uses the same threshold: an
// immediate one-buffer restart repeatedly underruns when the renderer submits
// audio less often than the device consumes it and is heard as crackling.
class AudioOutputStartPolicy final {
public:
  explicit AudioOutputStartPolicy(std::size_t startup_buffers) noexcept
      : startup_buffers_(std::max<std::size_t>(1U, startup_buffers)) {}

  [[nodiscard]] bool shouldStart(std::size_t queued_buffers,
                                 bool playing) noexcept {
    if (playing) {
      started_ = true;
      return false;
    }
    if (queued_buffers == 0U) {
      return false;
    }
    if (queued_buffers >= startup_buffers_) {
      started_ = true;
      return true;
    }
    return false;
  }

  void reset() noexcept { started_ = false; }
  [[nodiscard]] bool started() const noexcept { return started_; }

private:
  std::size_t startup_buffers_{};
  bool started_{};
};

// OpenAL reports every buffer attached to a stopped source as processed. A
// recovery queue therefore needs one explicit drain of the exhausted playback
// generation followed by a protected prebuffer phase; otherwise every fresh
// block is recycled before the source can reach its restart threshold.
class AudioOutputRecyclePolicy final {
public:
  [[nodiscard]] bool shouldRecycle(bool playing) noexcept {
    if (playing) {
      prebuffering_ = false;
      return true;
    }
    if (prebuffering_) {
      return false;
    }
    prebuffering_ = true;
    return true;
  }

  void playbackStarted() noexcept { prebuffering_ = false; }
  void reset() noexcept { prebuffering_ = true; }
  [[nodiscard]] bool prebuffering() const noexcept { return prebuffering_; }
  [[nodiscard]] bool
  shouldDrainStoppedGeneration(bool playing,
                               bool has_processed_buffers) const noexcept {
    return !playing && has_processed_buffers && !prebuffering_;
  }

private:
  bool prebuffering_{true};
};

// Smooths host-owned source gain without touching queued PCM. A stopped
// source adopts the target immediately, while a playing source moves by a
// bounded percentage on each presentation update.
class AudioOutputGainPolicy final {
public:
  explicit AudioOutputGainPolicy(std::uint8_t initial_percent = 100U,
                                 std::uint8_t maximum_step = 5U) noexcept
      : current_percent_(clamp(initial_percent)),
        target_percent_(current_percent_),
        maximum_step_(std::max<std::uint8_t>(1U, clamp(maximum_step))) {}

  void setTargetPercent(std::uint8_t percent) noexcept {
    target_percent_ = clamp(percent);
  }

  [[nodiscard]] std::uint8_t advance(bool playing) noexcept {
    if (!playing) {
      current_percent_ = target_percent_;
      return current_percent_;
    }
    if (current_percent_ < target_percent_) {
      const auto remaining =
          static_cast<std::uint8_t>(target_percent_ - current_percent_);
      current_percent_ = static_cast<std::uint8_t>(
          current_percent_ + std::min(maximum_step_, remaining));
    } else if (current_percent_ > target_percent_) {
      const auto remaining =
          static_cast<std::uint8_t>(current_percent_ - target_percent_);
      current_percent_ = static_cast<std::uint8_t>(
          current_percent_ - std::min(maximum_step_, remaining));
    }
    return current_percent_;
  }

  [[nodiscard]] std::uint8_t currentPercent() const noexcept {
    return current_percent_;
  }
  [[nodiscard]] std::uint8_t targetPercent() const noexcept {
    return target_percent_;
  }
  [[nodiscard]] float gain() const noexcept {
    return static_cast<float>(current_percent_) / 100.0F;
  }

private:
  [[nodiscard]] static constexpr std::uint8_t
  clamp(std::uint8_t percent) noexcept {
    return percent > 100U ? 100U : percent;
  }

  std::uint8_t current_percent_{};
  std::uint8_t target_percent_{};
  std::uint8_t maximum_step_{1U};
};

struct RuntimeHostIterationPolicy final {
  bool present_runtime_frame{true};
  bool refresh_retained_display{};
};

[[nodiscard]] inline constexpr RuntimeHostIterationPolicy
runtimeHostIterationPolicy(std::size_t guest_steps) noexcept {
  return RuntimeHostIterationPolicy{true, guest_steps == 0U};
}

// OpenAL consumes a 44.1 kHz stream in wall-clock time, while an overloaded
// interpreter produces one 735-frame SPU quantum per completed guest tick.
// Follow the measured guest clock so a transient CPU overload stretches the
// existing FIFO instead of underrunning, resetting and replaying buffers.
class RuntimeAudioPlaybackRatePolicy final {
public:
  explicit RuntimeAudioPlaybackRatePolicy(
      double guest_frames_per_second = 60.0,
      double minimum_playback_rate = 0.5) noexcept
      : guest_frames_per_second_(guest_frames_per_second),
        minimum_playback_rate_(std::clamp(minimum_playback_rate, 0.1, 1.0)) {}

  [[nodiscard]] double advance(double elapsed_seconds,
                               std::size_t guest_steps) noexcept {
    if (!std::isfinite(elapsed_seconds) || elapsed_seconds < 0.0) {
      elapsed_seconds = 0.0;
    }
    elapsed_seconds_ += elapsed_seconds;
    if (guest_steps == 0U) {
      return playback_rate_;
    }
    if (!observed_first_tick_) {
      observed_first_tick_ = true;
      elapsed_seconds_ = 0.0;
      return playback_rate_;
    }
    if (!std::isfinite(guest_frames_per_second_) ||
        guest_frames_per_second_ <= 0.0 || elapsed_seconds_ <= 0.0) {
      elapsed_seconds_ = 0.0;
      return playback_rate_;
    }

    const auto measured_rate = static_cast<double>(guest_steps) /
                               (elapsed_seconds_ * guest_frames_per_second_);
    elapsed_seconds_ = 0.0;
    const auto target = std::clamp(measured_rate, minimum_playback_rate_, 1.0);
    // A quarter-step low-pass converges before the startup queue can drain,
    // but avoids audible pitch flutter from timer quantisation.
    playback_rate_ += (target - playback_rate_) * 0.25;
    return playback_rate_;
  }

  [[nodiscard]] double playbackRate() const noexcept { return playback_rate_; }

private:
  double guest_frames_per_second_{60.0};
  double minimum_playback_rate_{0.5};
  double elapsed_seconds_{};
  double playback_rate_{1.0};
  bool observed_first_tick_{};
};

// Keeps the emulated retail clock at 60 Hz while presentation runs at an
// independent monitor/software cadence. A bounded backlog preserves transient
// presentation stalls without turning an arbitrarily long pause into an
// unbounded CPU/audio catch-up burst. Each host iteration remains separately
// capped, so recovery is gradual rather than a one-frame fast-forward.
inline constexpr std::size_t runtime_guest_maximum_catch_up_steps = 4U;

[[nodiscard]] inline constexpr std::size_t
runtimeGuestCatchUpStepsForPresentation(
    std::uint32_t presentation_frames_per_second) noexcept {
  if (presentation_frames_per_second == 0U ||
      presentation_frames_per_second >= 60U) {
    return 1U;
  }
  return std::clamp<std::size_t>((60U + presentation_frames_per_second - 1U) /
                                     presentation_frames_per_second,
                                 1U, runtime_guest_maximum_catch_up_steps);
}

class RuntimeGuestCadencePolicy final {
public:
  explicit RuntimeGuestCadencePolicy(
      double guest_frames_per_second = 60.0,
      std::size_t maximum_catch_up_steps = runtime_guest_maximum_catch_up_steps,
      std::size_t maximum_backlog_steps = 60U) noexcept
      : guest_frames_per_second_(guest_frames_per_second),
        maximum_catch_up_steps_(
            std::max<std::size_t>(maximum_catch_up_steps, 1U)),
        maximum_backlog_steps_(
            std::max(maximum_backlog_steps, maximum_catch_up_steps_)) {
    if (valid()) {
      accumulated_seconds_ = frameSeconds();
    }
  }

  [[nodiscard]] bool valid() const noexcept {
    return std::isfinite(guest_frames_per_second_) &&
           guest_frames_per_second_ > 0.0 &&
           guest_frames_per_second_ <= 1'000.0;
  }

  [[nodiscard]] std::size_t advance(double elapsed_seconds) noexcept {
    late_recovery_started_for_last_advance_ = false;
    suppress_audio_for_last_advance_ = false;
    if (!valid()) {
      return 0U;
    }
    if (!std::isfinite(elapsed_seconds) || elapsed_seconds < 0.0) {
      elapsed_seconds = 0.0;
    }
    maximum_elapsed_seconds_ =
        std::max(maximum_elapsed_seconds_, elapsed_seconds);
    const auto frame_seconds = frameSeconds();
    const auto epsilon = frame_seconds * 1.0e-9;
    const auto maximum_accumulated =
        frame_seconds * static_cast<double>(maximum_backlog_steps_);
    const auto backlog_before_advance = accumulated_seconds_;
    const auto accumulated = accumulated_seconds_ + elapsed_seconds;
    if (accumulated > maximum_accumulated) {
      dropped_seconds_ += accumulated - maximum_accumulated;
    }
    accumulated_seconds_ = std::min(accumulated, maximum_accumulated);
    const auto accumulated_steps = static_cast<std::size_t>(
        std::floor((accumulated_seconds_ + epsilon) / frame_seconds));
    if (!late_recovery_active_ && accumulated_steps > maximum_catch_up_steps_) {
      late_recovery_active_ = true;
      late_recovery_started_for_last_advance_ = true;
      ++late_recovery_count_;
    }
    const auto steps = std::min(accumulated_steps, maximum_catch_up_steps_);
    accumulated_seconds_ -= static_cast<double>(steps) * frame_seconds;
    accumulated_seconds_ = std::max(accumulated_seconds_, 0.0);
    // Suppress stale catch-up PCM only while this batch actually reduces the
    // pre-existing backlog. At a sustained 15 Hz presentation cadence four
    // new guest frames arrive while the four-step cap consumes four; keeping
    // suppression active there would drain current PCM forever. The batch
    // that starts recovery is always stale and remains suppressed.
    const auto recovery_progressed =
        accumulated_seconds_ + epsilon < backlog_before_advance;
    const auto recovery_stalled =
        late_recovery_active_ && !late_recovery_started_for_last_advance_ &&
        steps != 0U &&
        std::abs(accumulated_seconds_ - backlog_before_advance) <= epsilon;
    suppress_audio_for_last_advance_ =
        late_recovery_active_ && steps != 0U &&
        (late_recovery_started_for_last_advance_ || recovery_progressed);
    // If arriving work exactly consumes the catch-up budget, recovery can
    // never finish. Abandon and account for the old debt after the first
    // current batch so later stalls can arm a fresh audio reset.
    if (recovery_stalled) {
      dropped_seconds_ += accumulated_seconds_;
      accumulated_seconds_ = 0.0;
    }
    if (late_recovery_active_ &&
        accumulated_seconds_ + epsilon < frame_seconds) {
      late_recovery_active_ = false;
    }
    return steps;
  }

  [[nodiscard]] double backlogSeconds() const noexcept {
    return accumulated_seconds_;
  }
  [[nodiscard]] double droppedSeconds() const noexcept {
    return dropped_seconds_;
  }
  [[nodiscard]] double maximumElapsedSeconds() const noexcept {
    return maximum_elapsed_seconds_;
  }
  [[nodiscard]] bool lateRecoveryStartedForLastAdvance() const noexcept {
    return late_recovery_started_for_last_advance_;
  }
  [[nodiscard]] bool suppressAudioForLastAdvance() const noexcept {
    return suppress_audio_for_last_advance_;
  }
  [[nodiscard]] bool lateRecoveryActive() const noexcept {
    return late_recovery_active_;
  }
  [[nodiscard]] std::uint64_t lateRecoveryCount() const noexcept {
    return late_recovery_count_;
  }

private:
  [[nodiscard]] double frameSeconds() const noexcept {
    return 1.0 / guest_frames_per_second_;
  }

  double guest_frames_per_second_{60.0};
  double accumulated_seconds_{};
  double dropped_seconds_{};
  double maximum_elapsed_seconds_{};
  std::size_t maximum_catch_up_steps_{4U};
  std::size_t maximum_backlog_steps_{60U};
  std::uint64_t late_recovery_count_{};
  bool late_recovery_active_{};
  bool late_recovery_started_for_last_advance_{};
  bool suppress_audio_for_last_advance_{};
};

// Drains a bounded producer in FIFO order. The callback contract is kept
// independent of OpenAL so runtime audio can be regression-tested headlessly.
template <typename Frame, typename Drain, typename Submit>
[[nodiscard]] bool drainAudioFrames(std::span<Frame> scratch, Drain &&drain,
                                    Submit &&submit) {
  if (scratch.empty()) {
    return false;
  }
  for (;;) {
    const auto count = static_cast<std::size_t>(drain(scratch));
    if (count == 0U) {
      return true;
    }
    if (count > scratch.size()) {
      return false;
    }
    submit(std::span<const Frame>{scratch.data(), count});
  }
}

enum class RuntimeAudioDrainMode : std::uint8_t {
  queue,
  discard,
};

// Always consumes the guest FIFO. Only the realtime sink routing changes: a
// late catch-up batch has already elapsed on the wall clock and must not be
// queued behind the new scene's audio.
template <typename Frame, typename Drain, typename Submit>
[[nodiscard]] bool drainRuntimeAudioFrames(std::span<Frame> scratch,
                                           RuntimeAudioDrainMode mode,
                                           Drain &&drain, Submit &&submit) {
  return drainAudioFrames(scratch, std::forward<Drain>(drain),
                          [&](std::span<const Frame> frames) {
                            if (mode == RuntimeAudioDrainMode::queue) {
                              submit(frames);
                            }
                          });
}

// Uses one absolute movie clock instead of sleeping for a full frame after
// every decode/upload.  The latter accumulates decoder and GPU upload time,
// making STR video drift behind XA audio over long sequences.
class MovieFrameTimingPolicy final {
public:
  explicit MovieFrameTimingPolicy(double frames_per_second) noexcept
      : frames_per_second_(frames_per_second) {}

  [[nodiscard]] bool valid() const noexcept {
    return std::isfinite(frames_per_second_) && frames_per_second_ > 0.0 &&
           frames_per_second_ <= 120.0;
  }

  [[nodiscard]] double
  frameEndSeconds(double frame_timestamp_seconds,
                  std::optional<double> next_frame_timestamp_seconds) noexcept {
    if (!valid()) {
      return 0.0;
    }
    const auto timestamp_valid = [](double timestamp) noexcept {
      return std::isfinite(timestamp) && timestamp >= 0.0;
    };
    const auto frame_step = 1.0 / frames_per_second_;
    const auto fallback = last_deadline_seconds_ + frame_step;
    if (!timestamp_origin_seconds_) {
      if (timestamp_valid(frame_timestamp_seconds)) {
        timestamp_origin_seconds_ = frame_timestamp_seconds;
      } else if (next_frame_timestamp_seconds &&
                 timestamp_valid(*next_frame_timestamp_seconds)) {
        timestamp_origin_seconds_ = *next_frame_timestamp_seconds - fallback;
      }
    }
    auto deadline = fallback;
    if (timestamp_origin_seconds_ && next_frame_timestamp_seconds &&
        timestamp_valid(*next_frame_timestamp_seconds)) {
      const auto timestamp_deadline =
          *next_frame_timestamp_seconds - *timestamp_origin_seconds_;
      constexpr auto maximum_timestamp_step_frames = 4.0;
      const auto timestamp_step = timestamp_deadline - last_deadline_seconds_;
      if (std::isfinite(timestamp_deadline) && timestamp_step > 0.0 &&
          timestamp_step <= frame_step * maximum_timestamp_step_frames) {
        deadline = timestamp_deadline;
      } else {
        // A repeated/backwards PTS or a discontinuity must not freeze the
        // last decoded image. Rebase at the deterministic fixed-rate
        // deadline so later valid timestamps can resume absolute pacing.
        timestamp_origin_seconds_ = *next_frame_timestamp_seconds - fallback;
      }
    }
    last_deadline_seconds_ = deadline;
    return deadline;
  }

private:
  double frames_per_second_{};
  std::optional<double> timestamp_origin_seconds_;
  double last_deadline_seconds_{};
};

} // namespace sf::platform
