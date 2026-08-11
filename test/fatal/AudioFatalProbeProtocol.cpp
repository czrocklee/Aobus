// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/AudioFatalProbeProtocol.h"

#include <array>
#include <span>

namespace ao::audio::test
{
  std::span<AudioFatalProbeExpectation const> audioFatalProbeExpectations() noexcept
  {
    static constexpr auto kExpectations = std::array{
      AudioFatalProbeExpectation{"rt-ring-overflow", "category=realtime-invariant", "RT signal ring capacity exceeded"},
      AudioFatalProbeExpectation{
        "invalid-backend-third-push", "category=realtime-invariant", "RT signal ring capacity exceeded"},
      AudioFatalProbeExpectation{"timeline-owner-overwrite", "category=invariant", "lookahead owner must be empty"},
      AudioFatalProbeExpectation{
        "decoder-factory-null-success", "category=invariant", "Decoder factory succeeded without a session"},
      AudioFatalProbeExpectation{
        "queue-worker-joinable", "category=invariant", "must stop the queue before destruction"},
      AudioFatalProbeExpectation{
        "queue-worker-running", "category=invariant", "worker must exit before queue destruction"},
      AudioFatalProbeExpectation{
        "queue-playback-events", "category=invariant", "must clear playback events before destruction"},
      AudioFatalProbeExpectation{"queue-rt-signals", "category=invariant", "must drain RT signals before destruction"},
      AudioFatalProbeExpectation{
        "engine-event-thread-exception", "category=unhandled-exception", "engine event thread: probe exception"},
#ifdef __linux__
      AudioFatalProbeExpectation{
        "platform-graph-observer-exception", "category=unhandled-exception", "ALSA graph observer: probe exception"},
#endif
#ifdef _WIN32
      AudioFatalProbeExpectation{
        "platform-graph-observer-exception", "category=unhandled-exception", "WASAPI graph observer: probe exception"},
      AudioFatalProbeExpectation{
        "wasapi-device-observer-exception", "category=unhandled-exception", "WASAPI device observer: probe exception"},
#endif
    };

    return kExpectations;
  }
} // namespace ao::audio::test
