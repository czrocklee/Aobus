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
      AudioFatalProbeExpectation{"platform-graph-observer-exception",
                                 "category=unhandled-exception",
                                 "audio backend initial graph observer: probe exception"},
#ifdef _WIN32
      AudioFatalProbeExpectation{
        "wasapi-device-observer-exception", "category=unhandled-exception", "WASAPI device observer: probe exception"},
#endif
    };

    return kExpectations;
  }

#ifdef __APPLE__
  std::span<AudioFatalProbeExpectation const> coreAudioFatalProbeExpectations() noexcept
  {
    static constexpr auto kExpectations = std::array{
      AudioFatalProbeExpectation{"coreaudio-open-from-route-ready",
                                 "category=expects",
                                 "Cannot open Core Audio backend from its RenderTarget callback"},
      AudioFatalProbeExpectation{"coreaudio-close-from-route-ready",
                                 "category=expects",
                                 "Cannot close Core Audio backend from its RenderTarget callback"},
      AudioFatalProbeExpectation{"coreaudio-open-from-format-changed",
                                 "category=expects",
                                 "Cannot open Core Audio backend from its RenderTarget callback"},
      AudioFatalProbeExpectation{"coreaudio-close-from-format-changed",
                                 "category=expects",
                                 "Cannot close Core Audio backend from its RenderTarget callback"},
      AudioFatalProbeExpectation{"coreaudio-open-from-graph-callback",
                                 "category=expects",
                                 "Cannot open Core Audio backend from its RenderTarget callback"},
      AudioFatalProbeExpectation{"coreaudio-close-from-graph-callback",
                                 "category=expects",
                                 "Cannot close Core Audio backend from its RenderTarget callback"},
      AudioFatalProbeExpectation{"coreaudio-open-from-render-pcm",
                                 "category=expects",
                                 "Cannot open Core Audio backend from its RenderTarget callback"},
      AudioFatalProbeExpectation{"coreaudio-close-from-render-pcm",
                                 "category=expects",
                                 "Cannot close Core Audio backend from its RenderTarget callback"},
      AudioFatalProbeExpectation{"coreaudio-open-from-drain-complete",
                                 "category=expects",
                                 "Cannot open Core Audio backend from its RenderTarget callback"},
      AudioFatalProbeExpectation{"coreaudio-close-from-drain-complete",
                                 "category=expects",
                                 "Cannot close Core Audio backend from its RenderTarget callback"},
    };

    return kExpectations;
  }
#endif
} // namespace ao::audio::test
