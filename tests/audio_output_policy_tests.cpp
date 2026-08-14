#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/platform/audio_output_policy.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

void testStartupAndStableRecovery() {
  sf::platform::AudioOutputStartPolicy policy{2U};
  require(!policy.shouldStart(0U, false) && !policy.shouldStart(1U, false),
          "Audio output started before its initial prebuffer");
  require(policy.shouldStart(2U, false) && policy.started(),
          "Audio output did not start after one complete guest tick");
  require(!policy.shouldStart(2U, true),
          "Playing audio source was redundantly restarted");
  require(!policy.shouldStart(1U, false) && policy.shouldStart(2U, false),
          "Underrun recovery restarted from an unstable one-buffer queue");

  policy.reset();
  require(!policy.started() && !policy.shouldStart(1U, false),
          "Explicit reset retained the previous recovery state");
}

void testCallbackPrebufferCoversOneSlicePhaseJitter() {
  constexpr auto sample_rate = std::uint32_t{44'100U};
  constexpr auto device_refresh_hz = std::uint32_t{15U};
  constexpr auto producer_hz = std::uint32_t{120U};
  constexpr auto ring_capacity = std::size_t{22'050U};
  constexpr auto continuous_jitter_reserve = std::size_t{8U * 128U};
  constexpr auto device_frames = std::size_t{2'940U};
  constexpr auto seven_slice_frames =
      std::size_t{static_cast<std::uint64_t>(sample_rate) * 7U / producer_hz};
  constexpr auto sixteen_slice_frames =
      std::size_t{static_cast<std::uint64_t>(sample_rate) * 16U / producer_hz};
  constexpr auto nine_slice_frames = sixteen_slice_frames - seven_slice_frames;

  // The logged failing device exposed refresh=15 and ring=2940. The old
  // one-period threshold is emptied by one callback; if that callback runs
  // before the eighth 120 Hz producer slice, exactly 367/368 frames are
  // missing and OpenAL inserts silence.
  sf::platform::AudioFrameRing<int> old_ring{ring_capacity};
  std::vector<int> old_start(device_frames, 1);
  std::vector<int> callback(device_frames);
  std::vector<int> early_production(seven_slice_frames, 2);
  require(old_ring.push(old_start) == old_start.size() &&
              old_ring.pop(callback) == callback.size() &&
              old_ring.push(early_production) == early_production.size() &&
              old_ring.pop(callback) == seven_slice_frames,
          "Logged one-period callback starvation was not reproduced");

  const auto start_frames = sf::platform::audioCallbackStartFrames(
      continuous_jitter_reserve, sample_rate, device_refresh_hz, ring_capacity);
  sf::platform::AudioFrameRing<int> recovered_ring{ring_capacity};
  std::vector<int> initial_device_period(device_frames, 3);
  require(recovered_ring.push(initial_device_period) == device_frames &&
              !sf::platform::audioCallbackRecoveryReady(
                  true, recovered_ring.size(), start_frames),
          "Callback recovery consumed the one-period ring without cushion");
  std::vector<int> reserve(start_frames - device_frames, 4);
  require(recovered_ring.push(reserve) == reserve.size() &&
              sf::platform::audioCallbackRecoveryReady(
                  true, recovered_ring.size(), start_frames) &&
              recovered_ring.pop(callback) == callback.size(),
          "Callback recovery did not wait for its jitter high-water mark");

  std::vector<int> late_production(nine_slice_frames, 5);
  for (auto pair = 0U; pair < 32U; ++pair) {
    require(recovered_ring.push(early_production) == early_production.size() &&
                recovered_ring.pop(callback) == callback.size() &&
                recovered_ring.push(late_production) ==
                    late_production.size() &&
                recovered_ring.pop(callback) == callback.size(),
            "Recovered callback ring repeated phase-jitter starvation");
  }
  require(recovered_ring.size() == continuous_jitter_reserve,
          "Callback recovery did not preserve its steady jitter cushion");

  const auto refresh_60 = sf::platform::audioCallbackStartFrames(
      continuous_jitter_reserve, sample_rate, 60U, ring_capacity);
  const auto refresh_120 = sf::platform::audioCallbackStartFrames(
      continuous_jitter_reserve, sample_rate, 120U, ring_capacity);
  require(start_frames * 1000U / sample_rate < 100U &&
              refresh_60 * 1000U / sample_rate < 50U &&
              refresh_120 * 1000U / sample_rate < 40U,
          "Callback jitter reserve exceeded the bounded startup latency");
  require(sf::platform::audioCallbackStartFrames(
              continuous_jitter_reserve, sample_rate, 1U, ring_capacity) ==
                  ring_capacity &&
              sf::platform::audioCallbackStartFrames(
                  continuous_jitter_reserve, sample_rate, 0U, ring_capacity) ==
                  continuous_jitter_reserve,
          "Callback high-water did not clamp or handle unknown refresh");
  require(sf::platform::audioStarvationDiagnosticDue(0U, 1U) &&
              !sf::platform::audioStarvationDiagnosticDue(1U, 2U) &&
              !sf::platform::audioStarvationDiagnosticDue(2U, 63U) &&
              sf::platform::audioStarvationDiagnosticDue(63U, 64U) &&
              !sf::platform::audioStarvationDiagnosticDue(64U, 65U) &&
              sf::platform::audioStarvationDiagnosticDue(65U, 128U),
          "Callback starvation diagnostics are not rate-limited");
}

void testCallbackRecoveryCoversAtomicProducerPhase() {
  constexpr auto sample_rate = std::uint32_t{44'100U};
  constexpr auto device_refresh_hz = std::uint32_t{15U};
  constexpr auto ring_capacity = std::size_t{22'050U};
  constexpr auto jitter_reserve = std::size_t{8U * 128U};
  constexpr auto callback_frames = std::size_t{2'940U};
  constexpr auto maximum_producer_batch =
      std::size_t{4U * ((sample_rate + 60U - 1U) / 60U)};
  static_assert(maximum_producer_batch == callback_frames);

  const auto initial_start = sf::platform::audioCallbackStartFrames(
      jitter_reserve, sample_rate, device_refresh_hz, ring_capacity);
  const auto recovery_start = sf::platform::audioCallbackRecoveryFrames(
      initial_start, callback_frames, maximum_producer_batch, ring_capacity);
  require(initial_start == callback_frames + jitter_reserve &&
              recovery_start == callback_frames + maximum_producer_batch,
          "Initial and starvation callback thresholds were not separated");

  sf::platform::AudioFrameRing<int> ring{ring_capacity};
  std::vector<int> recovery(recovery_start, 1);
  std::vector<int> callback(callback_frames);
  std::vector<int> producer(maximum_producer_batch, 2);
  require(ring.push(recovery) == recovery.size() &&
              ring.pop(callback) == callback.size() &&
              ring.size() == maximum_producer_batch,
          "Recovery did not retain one atomic 15 Hz producer batch");
  for (auto period = 0U; period < 64U; ++period) {
    if ((period & 1U) == 0U) {
      require(ring.pop(callback) == callback.size() &&
                  ring.push(producer) == producer.size(),
              "Callback-before-producer phase starved");
    } else {
      require(ring.push(producer) == producer.size() &&
                  ring.pop(callback) == callback.size(),
              "Producer-before-callback phase starved");
    }
    require(ring.size() == maximum_producer_batch,
            "Atomic producer reserve drifted after recovery");
  }

  constexpr auto larger_request = std::size_t{4'096U};
  const auto larger_start = sf::platform::audioCallbackRecoveryFrames(
      initial_start, larger_request, maximum_producer_batch, ring_capacity);
  sf::platform::AudioFrameRing<int> larger_ring{ring_capacity};
  std::vector<int> larger_recovery(larger_start, 3);
  std::vector<int> larger_callback(larger_request);
  std::vector<int> pre_boundary(larger_request - maximum_producer_batch, 4);
  require(larger_start == larger_request + maximum_producer_batch &&
              larger_ring.push(larger_recovery) == larger_recovery.size() &&
              larger_ring.pop(larger_callback) == larger_callback.size() &&
              larger_ring.push(pre_boundary) == pre_boundary.size() &&
              larger_ring.pop(larger_callback) == larger_callback.size() &&
              larger_ring.push(producer) == producer.size() &&
              larger_ring.size() == maximum_producer_batch,
          "Observed larger callback request immediately re-starved");
  require(!sf::platform::audioCallbackStartupReady(initial_start, initial_start,
                                                   larger_request) &&
              sf::platform::audioCallbackStartupReady(
                  larger_request, initial_start, larger_request),
          "Reset startup ignored the learned callback request");

  require(sf::platform::audioCallbackRecoveryFrames(
              initial_start, ring_capacity, maximum_producer_batch,
              ring_capacity) == ring_capacity,
          "Callback recovery threshold exceeded its bounded ring");
}

void testContinuousTimelineOverflowRebuffersCurrentPcm() {
  using sf::platform::AudioTimelineCapacityAction;
  using sf::platform::audioTimelineCapacityAction;

  require(audioTimelineCapacityAction(700U, 300U, 1'000U, true) ==
                  AudioTimelineCapacityAction::accept &&
              audioTimelineCapacityAction(701U, 300U, 1'000U, true) ==
                  AudioTimelineCapacityAction::rebuffer,
          "Continuous audio did not rebuffer exactly at FIFO overflow");
  require(audioTimelineCapacityAction(701U, 300U, 1'000U, false) ==
              AudioTimelineCapacityAction::reject,
          "One-shot audio silently discarded an earlier cue");
  require(audioTimelineCapacityAction(0U, 1'001U, 1'000U, true) ==
                  AudioTimelineCapacityAction::reject &&
              audioTimelineCapacityAction(1'500U, 1U, 1'000U, true) ==
                  AudioTimelineCapacityAction::rebuffer,
          "Audio FIFO accepted an oversized block or mishandled a saturated "
          "staging count");
}

void testRealtimeFrameRingWrapsWithoutBlockingOrReordering() {
  sf::platform::AudioFrameRing<int> ring{5U};
  const std::array first{1, 2, 3, 4};
  require(ring.push(first) == first.size() && ring.size() == first.size(),
          "Realtime audio ring did not accept its initial frames");

  std::array<int, 3U> head{};
  require(ring.pop(head) == head.size() && head == std::array<int, 3U>{1, 2, 3},
          "Realtime audio ring reordered its initial frames");

  const std::array wrapped{5, 6, 7, 8, 9};
  require(ring.push(wrapped) == 4U && ring.size() == ring.capacity(),
          "Realtime audio ring did not bound a wrapped producer burst");
  std::array<int, 6U> tail{};
  require(ring.pop(tail) == 5U &&
              std::ranges::equal(std::span{tail}.first(5U),
                                 std::array<int, 5U>{4, 5, 6, 7, 8}),
          "Realtime audio ring lost ordering across its wrap point");

  require(ring.push(std::array{10, 11, 12}) == 3U, "Ring refill failed");
  ring.clear();
  require(ring.size() == 0U && ring.pop(tail) == 0U,
          "Realtime audio ring retained frames across a stream reset");
}

void testStoppedSourcePrebufferIsNotRecycled() {
  sf::platform::AudioOutputRecyclePolicy policy;
  require(!policy.shouldRecycle(false) && policy.prebuffering(),
          "Initial OpenAL prebuffer was treated as played data");
  policy.playbackStarted();
  require(!policy.prebuffering() && policy.shouldRecycle(true),
          "Playing OpenAL source stopped recycling completed buffers");
  require(policy.shouldDrainStoppedGeneration(false, true),
          "A stopped live generation was not eligible for recovery draining");
  require(policy.shouldRecycle(false) && policy.prebuffering(),
          "First stopped-source observation did not drain the old generation");
  require(!policy.shouldDrainStoppedGeneration(false, true),
          "Fresh recovery buffers were mistaken for the stopped generation");
  require(!policy.shouldRecycle(false),
          "Fresh recovery buffers were recycled before restart");
  policy.playbackStarted();
  require(policy.shouldRecycle(true),
          "Recovered source did not return to normal recycling");
  policy.reset();
  require(policy.prebuffering() && !policy.shouldRecycle(false),
          "Explicit reset did not protect the new prebuffer");
}

void testBoundedGainRamp() {
  sf::platform::AudioOutputGainPolicy policy{100U, 5U};
  policy.setTargetPercent(0U);
  require(policy.advance(true) == 95U && policy.currentPercent() == 95U,
          "Playing source gain jumped instead of ramping down");
  require(policy.advance(true) == 90U,
          "Playing source gain did not use its fixed downward step");
  require(policy.advance(false) == 0U && policy.gain() == 0.0F,
          "Stopped source did not adopt its target immediately");

  policy.setTargetPercent(12U);
  require(policy.advance(true) == 5U && policy.advance(true) == 10U &&
              policy.advance(true) == 12U,
          "Playing source gain overshot its upward target");
  policy.setTargetPercent(255U);
  require(policy.targetPercent() == 100U && policy.advance(false) == 100U &&
              policy.gain() == 1.0F,
          "Gain target was not clamped to OpenAL's normalized range");
}

void testRetailVolumeMapping() {
  require(sf::game::legacyRetailAudioVolumeFromPercent(0U) == 0U &&
              sf::game::legacyRetailAudioVolumeFromPercent(50U) == 64U &&
              sf::game::legacyRetailAudioVolumeFromPercent(100U) == 127U &&
              sf::game::legacyRetailAudioVolumeFromPercent(255U) == 127U &&
              sf::game::legacyRetailAudioVolumeToPercent(0U) == 0U &&
              sf::game::legacyRetailAudioVolumeToPercent(64U) == 50U &&
              sf::game::legacyRetailAudioVolumeToPercent(127U) == 100U &&
              sf::game::legacyRetailAudioVolumeToPercent(255U) == 100U,
          "Pause volume did not map to the retail 0..127 range");
  const sf::game::LegacyRetailAudioVolumes volumes{
      .sound_effects = 17U,
      .music = 43U,
      .voice_over = 71U,
  };
  require(volumes.valid() &&
              volumes.groups() == std::array<std::uint8_t, 3U>{17U, 43U, 71U},
          "Retail sound groups are not ordered as SFX/Music/Voice-over");
}

void testRuntimeGuestCadenceIsPresentationIndependent() {
  sf::platform::RuntimeGuestCadencePolicy high_refresh;
  auto guest_steps = high_refresh.advance(0.0);
  for (auto presentation = 0U; presentation < 240U; ++presentation) {
    guest_steps += high_refresh.advance(1.0 / 240.0);
  }
  require(guest_steps == 61U,
          "240 Hz presentation changed the 60 Hz guest cadence");

  sf::platform::RuntimeGuestCadencePolicy low_refresh;
  require(low_refresh.advance(0.0) == 1U &&
              low_refresh.advance(1.0 / 30.0) == 2U &&
              !low_refresh.lateRecoveryStartedForLastAdvance() &&
              !low_refresh.suppressAudioForLastAdvance() &&
              low_refresh.lateRecoveryCount() == 0U,
          "Normal 30 Hz two-step presentation triggered audio resync");

  sf::platform::RuntimeGuestCadencePolicy minimum_refresh;
  require(minimum_refresh.advance(0.0) == 1U &&
              minimum_refresh.advance(1.0 / 15.0) == 4U &&
              !minimum_refresh.lateRecoveryStartedForLastAdvance() &&
              !minimum_refresh.suppressAudioForLastAdvance(),
          "Normal 15 Hz four-step presentation triggered late recovery");

  sf::platform::RuntimeGuestCadencePolicy fractional_overrun;
  require(fractional_overrun.advance(0.0) == 1U &&
              fractional_overrun.advance(0.070) == 4U &&
              fractional_overrun.backlogSeconds() > 0.0 &&
              fractional_overrun.backlogSeconds() < 1.0 / 60.0 &&
              !fractional_overrun.lateRecoveryStartedForLastAdvance() &&
              !fractional_overrun.suppressAudioForLastAdvance() &&
              !fractional_overrun.lateRecoveryActive() &&
              fractional_overrun.lateRecoveryCount() == 0U,
          "A fractional 70 ms overrun reset or discarded current audio "
          "without a complete overdue guest frame");

  sf::platform::RuntimeGuestCadencePolicy low_latency{60.0, 1U, 1U};
  require(low_latency.advance(0.0) == 1U &&
              low_latency.advance(1.0 / 15.0) == 1U &&
              low_latency.backlogSeconds() < 1.0e-9 &&
              low_latency.droppedSeconds() > 0.049 &&
              low_latency.droppedSeconds() < 0.051 &&
              low_latency.advance(0.0) == 0U,
          "Low-latency cadence retained a self-sustaining catch-up batch");

  sf::platform::RuntimeGuestCadencePolicy retained_single_step{60.0, 1U, 2U};
  require(retained_single_step.advance(0.0) == 1U &&
              retained_single_step.advance(0.016) == 0U &&
              retained_single_step.backlogSeconds() > 0.0159 &&
              retained_single_step.advance(1.0 / 60.0) == 1U &&
              retained_single_step.backlogSeconds() > 0.0159 &&
              retained_single_step.advance(0.001) == 1U &&
              retained_single_step.droppedSeconds() < 1.0e-9,
          "Single-step cadence discarded the sub-frame debt needed to avoid "
          "a 60/40 Hz VSync alias");

  sf::platform::RuntimeGuestCadencePolicy bounded_stall;
  auto recovered_steps = bounded_stall.advance(0.0);
  recovered_steps += bounded_stall.advance(1.0);
  require(bounded_stall.lateRecoveryStartedForLastAdvance() &&
              bounded_stall.suppressAudioForLastAdvance() &&
              bounded_stall.lateRecoveryActive() &&
              bounded_stall.lateRecoveryCount() == 1U,
          "One-second stall did not enter late audio recovery");
  std::size_t largest_recovery_batch{};
  for (std::size_t recovery{}; recovery < 14U; ++recovery) {
    const auto batch = bounded_stall.advance(0.0);
    largest_recovery_batch = std::max(largest_recovery_batch, batch);
    recovered_steps += batch;
    require(bounded_stall.suppressAudioForLastAdvance(),
            "Late recovery exposed stale PCM before catching up");
  }
  require(recovered_steps == 61U && largest_recovery_batch <= 4U &&
              bounded_stall.advance(0.0) == 0U &&
              !bounded_stall.suppressAudioForLastAdvance() &&
              !bounded_stall.lateRecoveryActive() &&
              std::abs(bounded_stall.maximumElapsedSeconds() - 1.0) < 1.0e-9 &&
              bounded_stall.droppedSeconds() < 1.0e-9,
          "One-second presentation stall was lost or caught up in one burst");

  sf::platform::RuntimeGuestCadencePolicy overflow_stall;
  require(overflow_stall.advance(0.0) == 1U &&
              overflow_stall.advance(1.5) == 4U &&
              std::abs(overflow_stall.droppedSeconds() - 0.5) < 1.0e-9 &&
              overflow_stall.backlogSeconds() > 0.9,
          "Cadence backlog did not bound and account for an excessive stall");

  // Repeated 100 ms upload/presentation spikes used to discard 33 ms on
  // every sample (`maximum_catch_up_steps / 60`). Retaining that debt lets
  // the guest CD/SPU clock recover gradually without changing its 60 Hz
  // authoritative cadence.
  sf::platform::RuntimeGuestCadencePolicy upload_spikes;
  auto spike_steps = upload_spikes.advance(0.0);
  for (std::size_t spike{}; spike < 5U; ++spike) {
    spike_steps += upload_spikes.advance(0.1);
  }
  for (std::size_t recovery{}; recovery < 8U; ++recovery) {
    spike_steps += upload_spikes.advance(0.0);
  }
  require(spike_steps == 31U && upload_spikes.advance(0.0) == 0U &&
              upload_spikes.droppedSeconds() < 1.0e-9,
          "Transient screen-upload stalls permanently slowed guest time");
  require(upload_spikes.lateRecoveryCount() == 1U &&
              !upload_spikes.suppressAudioForLastAdvance(),
          "Repeated upload stalls retriggered or retained audio recovery");

  sf::platform::RuntimeGuestCadencePolicy invalid{
      std::numeric_limits<double>::quiet_NaN()};
  require(!invalid.valid() && invalid.advance(1.0) == 0U,
          "Invalid guest cadence produced emulation steps");

  sf::platform::RuntimeGuestCadencePolicy invalid_elapsed;
  require(invalid_elapsed.advance(0.0) == 1U &&
              invalid_elapsed.advance(
                  std::numeric_limits<double>::infinity()) == 0U &&
              invalid_elapsed.advance(-1.0) == 0U,
          "Invalid presentation delta advanced the guest clock");
}

void testRuntimePresentationIsGuestIndependent() {
  using sf::platform::RuntimeHostPresentationMode;
  using sf::platform::RuntimeVisualPublicationState;
  using sf::platform::RuntimeVisualPublicationTracker;

  RuntimeVisualPublicationTracker publications;
  const RuntimeVisualPublicationState initial{};
  require(publications.observe(initial, false),
          "First visual publication was treated as cached");
  require(!publications.observe(initial, false),
          "Unchanged empty guest tick dirtied presentation");

  auto display_flip = initial;
  display_flip.display_x = 384U;
  require(publications.observe(display_flip, false) &&
              !publications.observe(display_flip, false),
          "Display-page flip was not published exactly once");

  auto reset_epoch = display_flip;
  ++reset_epoch.command_buffer_epoch;
  require(publications.observe(reset_epoch, false) &&
              !publications.observe(reset_epoch, false),
          "GPU reset epoch was not published exactly once");
  require(publications.observe(reset_epoch, true),
          "Non-empty GP0 stream reused a cached presentation");

  auto display_disabled = reset_epoch;
  display_disabled.display_enabled = false;
  require(publications.observe(display_disabled, false),
          "GP1 display-disable did not dirty presentation");

  const auto idle = sf::platform::runtimeHostPresentationMode(false);
  require(idle == sf::platform::RuntimeHostPresentationMode::cached,
          "High-refresh presentation did not reuse an idle guest frame");

  const auto dirty = sf::platform::runtimeHostPresentationMode(true);
  require(dirty == RuntimeHostPresentationMode::render,
          "A new visual publication suppressed presentation");

  require(
      sf::platform::runtimeGuestCatchUpStepsForPresentation(0U) == 1U &&
          sf::platform::runtimeGuestCatchUpStepsForPresentation(30U) == 2U &&
          sf::platform::runtimeGuestCatchUpStepsForPresentation(60U) == 1U &&
          sf::platform::runtimeGuestCatchUpStepsForPresentation(120U) == 1U &&
          sf::platform::runtimeGuestCatchUpStepsForPresentation(240U) == 1U,
      "Presentation FPS changed the 60 Hz guest batching policy");
}

void testRuntimePresentationInterpolationClock() {
  const auto verify_cadence = [](std::uint32_t presentation_fps,
                                 std::span<const double> expected) {
    sf::platform::RuntimePresentationInterpolationClock clock;
    require(clock.valid() && clock.alpha() == 1.0,
            "Presentation interpolation did not start on a complete frame");
    clock.publishAuthoredFrame();
    require(clock.alpha() == 0.0,
            "A new authored frame did not begin a causal interval");
    for (const auto expected_alpha : expected) {
      const auto actual =
          clock.advance(1.0 / static_cast<double>(presentation_fps));
      require(std::abs(actual - expected_alpha) < 1.0e-9,
              "Presentation interpolation produced the wrong causal alpha");
    }
    require(clock.alpha() == 1.0 && clock.advance(1.0) == 1.0,
            "Presentation interpolation advanced past the current frame");
  };

  constexpr std::array cadence_30{1.0};
  constexpr std::array cadence_60{0.5, 1.0};
  constexpr std::array cadence_120{0.25, 0.5, 0.75, 1.0};
  constexpr std::array cadence_240{0.125, 0.25, 0.375, 0.5,
                                   0.625, 0.75, 0.875, 1.0};
  verify_cadence(30U, cadence_30);
  verify_cadence(60U, cadence_60);
  verify_cadence(120U, cadence_120);
  verify_cadence(240U, cadence_240);

  sf::platform::RuntimePresentationInterpolationClock hitch;
  hitch.publishAuthoredFrame();
  require(hitch.advance(0.2) == 1.0,
          "Presentation hitch did not clamp to the current authored frame");
  hitch.publishAuthoredFrame();
  require(hitch.advance(-1.0) == 0.0 &&
              hitch.advance(std::numeric_limits<double>::infinity()) == 0.0,
          "Invalid host delta changed presentation interpolation phase");
  hitch.reset();
  require(hitch.alpha() == 1.0,
          "Presentation interpolation reset exposed an empty history");

  sf::platform::RuntimePresentationInterpolationClock invalid{0.0};
  require(!invalid.valid() && invalid.advance(1.0 / 60.0) == 1.0,
          "Invalid authored cadence produced a partial presentation frame");
}

void testRuntimeAudioPlaybackRateTracksGuestClock() {
  sf::platform::RuntimeAudioPlaybackRatePolicy stable;
  static_cast<void>(stable.advance(0.0, 1U));
  for (auto frame = 0U; frame < 30U; ++frame) {
    static_cast<void>(stable.advance(1.0 / 60.0, 1U));
  }
  require(std::abs(stable.playbackRate() - 1.0) < 0.001,
          "Stable guest cadence changed the native playback rate");

  sf::platform::RuntimeAudioPlaybackRatePolicy overloaded;
  static_cast<void>(overloaded.advance(0.0, 1U));
  for (auto frame = 0U; frame < 30U; ++frame) {
    static_cast<void>(overloaded.advance(1.0 / 52.0, 1U));
  }
  require(overloaded.playbackRate() > 0.86 && overloaded.playbackRate() < 0.88,
          "Slow guest cadence did not slow the sink before FIFO starvation");

  for (auto frame = 0U; frame < 40U; ++frame) {
    static_cast<void>(overloaded.advance(1.0 / 60.0, 1U));
  }
  require(overloaded.playbackRate() > 0.999,
          "Recovered guest cadence left gameplay audio slowed");

  sf::platform::RuntimeAudioPlaybackRatePolicy idle_aware;
  static_cast<void>(idle_aware.advance(0.0, 1U));
  static_cast<void>(idle_aware.advance(1.0 / 120.0, 0U));
  static_cast<void>(idle_aware.advance(1.0 / 120.0, 1U));
  require(std::abs(idle_aware.playbackRate() - 1.0) < 0.001,
          "Idle host iteration was omitted from the guest audio clock");

  const auto retained =
      idle_aware.advance(std::numeric_limits<double>::infinity(), 0U);
  require(std::isfinite(retained),
          "Invalid host delta poisoned the audio playback rate");
}

void testTempoStretchPreservesPitchAndDuration() {
  struct StereoFrame {
    std::int16_t left{};
    std::int16_t right{};
  };
  constexpr auto sample_rate = 44'100.0;
  constexpr auto frequency = 440.0;
  constexpr auto tempo = 0.875;
  constexpr auto pi = 3.14159265358979323846;
  std::vector<StereoFrame> source(static_cast<std::size_t>(sample_rate * 2.0));
  for (std::size_t index{}; index < source.size(); ++index) {
    const auto sample = static_cast<std::int16_t>(
        std::sin(2.0 * pi * frequency * static_cast<double>(index) /
                 sample_rate) *
        16'000.0);
    source[index] = StereoFrame{sample, sample};
  }

  sf::platform::AudioTempoStretcher<StereoFrame> stretcher;
  std::vector<StereoFrame> output;
  for (std::size_t offset{}; offset < source.size(); offset += 735U) {
    const auto count = std::min<std::size_t>(735U, source.size() - offset);
    stretcher.process(
        std::span<const StereoFrame>{source}.subspan(offset, count), tempo,
        output);
  }
  const auto expected = static_cast<std::size_t>(source.size() / tempo);
  require(output.size() + 1'024U >= expected &&
              output.size() <= expected + 1'024U,
          "Tempo stretch produced the wrong duration");

  const auto begin = std::min<std::size_t>(2'048U, output.size());
  const auto end = output.size() > 2'048U ? output.size() - 2'048U : begin;
  std::size_t crossings{};
  for (auto index = begin + 1U; index < end; ++index) {
    crossings +=
        output[index - 1U].left <= 0 && output[index].left > 0 ? 1U : 0U;
  }
  const auto seconds = static_cast<double>(end - begin) / sample_rate;
  const auto measured = seconds > 0.0 ? crossings / seconds : 0.0;
  require(std::abs(measured - frequency) < 8.0,
          "Tempo stretch changed the signal pitch");
}

void testSaturatedLateRecoveryDoesNotPermanentlyMuteAudio() {
  sf::platform::RuntimeGuestCadencePolicy cadence;
  require(cadence.advance(0.0) == 1U,
          "Cadence did not emit its initial guest frame");

  auto queued_frames = std::size_t{};
  auto drained_frames = std::size_t{};
  auto sequence = 0;
  std::array<int, 3U> scratch{};
  const auto route_guest_steps = [&](std::size_t guest_steps) {
    std::vector<int> produced(guest_steps * 5U);
    for (auto &frame : produced) {
      frame = sequence++;
    }
    auto offset = std::size_t{};
    const auto mode = cadence.suppressAudioForLastAdvance()
                          ? sf::platform::RuntimeAudioDrainMode::discard
                          : sf::platform::RuntimeAudioDrainMode::queue;
    const auto valid = sf::platform::drainRuntimeAudioFrames(
        std::span<int>{scratch}, mode,
        [&](std::span<int> destination) {
          const auto count =
              std::min(destination.size(), produced.size() - offset);
          std::copy_n(produced.begin() + static_cast<std::ptrdiff_t>(offset),
                      count, destination.begin());
          offset += count;
          drained_frames += count;
          return count;
        },
        [&](std::span<const int> frames) { queued_frames += frames.size(); });
    require(valid && offset == produced.size(),
            "Runtime audio did not fully drain a cadence batch");
  };

  const auto stalled_steps = cadence.advance(1.0);
  require(stalled_steps == 4U && cadence.lateRecoveryStartedForLastAdvance() &&
              cadence.suppressAudioForLastAdvance(),
          "One-second stall did not reset and discard stale PCM");
  route_guest_steps(stalled_steps);

  for (auto presentation = 0U; presentation < 8U; ++presentation) {
    const auto guest_steps = cadence.advance(1.0 / 15.0);
    require(guest_steps == 4U && !cadence.lateRecoveryActive() &&
                !cadence.lateRecoveryStartedForLastAdvance() &&
                cadence.lateRecoveryCount() == 1U &&
                !cadence.suppressAudioForLastAdvance(),
            "Saturated late recovery permanently discarded current PCM");
    route_guest_steps(guest_steps);
  }
  require(cadence.droppedSeconds() > 0.9 && cadence.droppedSeconds() < 0.95 &&
              drained_frames == 180U && queued_frames == 160U,
          "Saturated late recovery did not resume current PCM routing");

  const auto second_stall_steps = cadence.advance(1.0);
  require(second_stall_steps == 4U && cadence.lateRecoveryActive() &&
              cadence.lateRecoveryStartedForLastAdvance() &&
              cadence.lateRecoveryCount() == 2U &&
              cadence.suppressAudioForLastAdvance(),
          "Saturated recovery hid a later presentation stall");
  route_guest_steps(second_stall_steps);
  require(drained_frames == 200U && queued_frames == 160U,
          "A later stall was not drained and discarded after rearming");
}

void testRuntimeAudioDrainPreservesEveryFrame() {
  std::vector<int> produced(9'000U);
  for (std::size_t index{}; index < produced.size(); ++index) {
    produced[index] = static_cast<int>(index);
  }
  std::array<int, 4'096U> scratch{};
  std::size_t offset{};
  std::vector<std::size_t> submitted_blocks;
  std::vector<int> submitted;

  const auto valid = sf::platform::drainAudioFrames(
      std::span<int>{scratch},
      [&](std::span<int> destination) {
        const auto count =
            std::min(destination.size(), produced.size() - offset);
        std::copy_n(produced.begin() + static_cast<std::ptrdiff_t>(offset),
                    count, destination.begin());
        offset += count;
        return count;
      },
      [&](std::span<const int> frames) {
        submitted_blocks.push_back(frames.size());
        submitted.insert(submitted.end(), frames.begin(), frames.end());
      });

  require(valid && offset == produced.size() && submitted == produced,
          "Runtime audio drain lost, duplicated, or reordered PCM frames");
  require(submitted_blocks == std::vector<std::size_t>{4'096U, 4'096U, 808U},
          "Runtime audio drain did not preserve its bounded block cadence");
}

void testRuntimeAudioDrainDiscardsLateRecoveryFrames() {
  std::vector<int> produced(9'000U, 7);
  std::array<int, 4'096U> scratch{};
  std::size_t offset{};
  std::size_t submitted{};
  const auto valid = sf::platform::drainRuntimeAudioFrames(
      std::span<int>{scratch}, sf::platform::RuntimeAudioDrainMode::discard,
      [&](std::span<int> destination) {
        const auto count =
            std::min(destination.size(), produced.size() - offset);
        std::copy_n(produced.begin() + static_cast<std::ptrdiff_t>(offset),
                    count, destination.begin());
        offset += count;
        return count;
      },
      [&](std::span<const int> frames) { submitted += frames.size(); });

  require(valid && offset == produced.size() && submitted == 0U,
          "Late recovery did not drain and discard every stale PCM frame");

  offset = 0U;
  std::vector<int> routed;
  const auto queue_valid = sf::platform::drainRuntimeAudioFrames(
      std::span<int>{scratch}, sf::platform::RuntimeAudioDrainMode::queue,
      [&](std::span<int> destination) {
        const auto count =
            std::min(destination.size(), produced.size() - offset);
        std::copy_n(produced.begin() + static_cast<std::ptrdiff_t>(offset),
                    count, destination.begin());
        offset += count;
        return count;
      },
      [&](std::span<const int> frames) {
        routed.insert(routed.end(), frames.begin(), frames.end());
      });
  require(queue_valid && offset == produced.size() && routed == produced,
          "Normal runtime audio routing lost or reordered PCM frames");
}

void testRuntimeAudioDrainRejectsInvalidProducerCounts() {
  std::array<int, 8U> scratch{};
  auto submissions = std::size_t{};
  const auto valid = sf::platform::drainAudioFrames(
      std::span<int>{scratch},
      [](std::span<int> destination) { return destination.size() + 1U; },
      [&](std::span<const int>) { ++submissions; });
  require(!valid && submissions == 0U,
          "Runtime audio drain accepted an over-capacity producer count");

  const auto empty_valid = sf::platform::drainAudioFrames(
      std::span<int>{}, [](std::span<int>) { return std::size_t{}; },
      [&](std::span<const int>) { ++submissions; });
  require(!empty_valid && submissions == 0U,
          "Runtime audio drain accepted an empty scratch buffer");
}

void testMovieFrameTimingUsesOneAbsoluteClock() {
  sf::platform::MovieFrameTimingPolicy ntsc_str{15.0};
  require(
      ntsc_str.valid() &&
          std::abs(ntsc_str.frameEndSeconds(10.0, 10.08) - 0.08) < 0.000'001 &&
          std::abs(ntsc_str.frameEndSeconds(10.08, 10.15) - 0.15) < 0.000'001,
      "STR frame pacing ignored decoded presentation timestamps");

  sf::platform::MovieFrameTimingPolicy missing_timestamps{15.0};
  require(std::abs(missing_timestamps.frameEndSeconds(0.0, std::nullopt) -
                   (1.0 / 15.0)) < 0.000'001 &&
              std::abs(missing_timestamps.frameEndSeconds(0.0, 0.0) -
                       (2.0 / 15.0)) < 0.000'001,
          "Missing or repeated STR timestamps broke fixed-rate fallback");

  require(!sf::platform::MovieFrameTimingPolicy{0.0}.valid() &&
              !sf::platform::MovieFrameTimingPolicy{
                  std::numeric_limits<double>::infinity()}
                   .valid() &&
              !sf::platform::MovieFrameTimingPolicy{121.0}.valid(),
          "Invalid STR frame rate was accepted");

  sf::platform::MovieFrameTimingPolicy discontinuous{10.0};
  require(std::abs(discontinuous.frameEndSeconds(5.0, 5.1) - 0.1) < 0.000'001 &&
              std::abs(discontinuous.frameEndSeconds(5.1, 50.0) - 0.2) <
                  0.000'001 &&
              std::abs(discontinuous.frameEndSeconds(50.0, 50.1) - 0.3) <
                  0.000'001,
          "Large STR PTS discontinuity froze or permanently desynchronized "
          "the movie clock");

  sf::platform::MovieFrameTimingPolicy nonpositive{10.0};
  require(std::abs(nonpositive.frameEndSeconds(3.0, 3.1) - 0.1) < 0.000'001 &&
              std::abs(nonpositive.frameEndSeconds(3.1, 3.0) - 0.2) <
                  0.000'001 &&
              std::abs(nonpositive.frameEndSeconds(3.0, 3.1) - 0.3) < 0.000'001,
          "Repeated or backwards STR PTS bypassed fixed-rate recovery");

  sf::platform::MovieFrameTimingPolicy nonfinite{10.0};
  require(std::abs(nonfinite.frameEndSeconds(
                       std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()) -
                   0.1) < 0.000'001 &&
              std::abs(nonfinite.frameEndSeconds(
                           std::numeric_limits<double>::quiet_NaN(), 7.0) -
                       0.2) < 0.000'001,
          "Non-finite STR PTS escaped deterministic fallback");
}

} // namespace

int main() {
  try {
    testStartupAndStableRecovery();
    testCallbackPrebufferCoversOneSlicePhaseJitter();
    testCallbackRecoveryCoversAtomicProducerPhase();
    testContinuousTimelineOverflowRebuffersCurrentPcm();
    testRealtimeFrameRingWrapsWithoutBlockingOrReordering();
    testStoppedSourcePrebufferIsNotRecycled();
    testBoundedGainRamp();
    testRetailVolumeMapping();
    testRuntimeGuestCadenceIsPresentationIndependent();
    testRuntimePresentationIsGuestIndependent();
    testRuntimePresentationInterpolationClock();
    testRuntimeAudioPlaybackRateTracksGuestClock();
    testTempoStretchPreservesPitchAndDuration();
    testSaturatedLateRecoveryDoesNotPermanentlyMuteAudio();
    testRuntimeAudioDrainPreservesEveryFrame();
    testRuntimeAudioDrainDiscardsLateRecoveryFrames();
    testRuntimeAudioDrainRejectsInvalidProducerCounts();
    testMovieFrameTimingUsesOneAbsoluteClock();
  } catch (const std::exception &error) {
    std::cerr << "audio output policy tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "audio output policy tests passed\n";
  return 0;
}
