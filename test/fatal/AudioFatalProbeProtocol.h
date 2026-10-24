// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <span>
#include <string_view>

namespace ao::audio::test
{
  struct AudioFatalProbeExpectation final
  {
    std::string_view scenario;
    std::string_view category;
    std::string_view context;
  };

  std::span<AudioFatalProbeExpectation const> audioFatalProbeExpectations() noexcept;

  // Common CallbackFence open-precondition expectations. These children call
  // only the single-threaded public fence API in the fatal probe process and
  // are available on every host; they never open an audio device.
  std::span<AudioFatalProbeExpectation const> callbackFenceFatalProbeExpectations() noexcept;

#ifdef __APPLE__
  // Apple-only native Core Audio reentry expectations. The all-host probe
  // runner keeps iterating audioFatalProbeExpectations() only; the Core
  // executable's CoreAudioBackendFatalProbeTest drives this span through the
  // same ao_audio_fatal_probe child entry.
  std::span<AudioFatalProbeExpectation const> coreAudioFatalProbeExpectations() noexcept;

  // Separate from the thirty native reentry expectations. These children call
  // CoreAudioRenderAdmission::open() only and do not open a device.
  std::span<AudioFatalProbeExpectation const> coreAudioRenderAdmissionFatalProbeExpectations() noexcept;

  // Device-free CoreAudioUnitRun start-precondition expectation. The child
  // constructs the production record with both constructor-injected
  // operations overridden, so it never calls the AudioToolbox SDK, opens a
  // device, or looks up a native component.
  std::span<AudioFatalProbeExpectation const> coreAudioUnitRunFatalProbeExpectations() noexcept;

  // Device-free CoreAudioUnitRun constructor-binding expectations. Each child
  // constructs the production record directly with exactly one of the two
  // constructor-injected operations empty; the other binding keeps its SDK
  // default and is never invoked.
  std::span<AudioFatalProbeExpectation const> coreAudioUnitRunConstructorFatalProbeExpectations() noexcept;

  // Device-free CoreAudioBackend constructor-binding expectations. Each child
  // constructs the backend through its four-argument constructor with a
  // value-only device identity and one empty unit operation; the runtime-state
  // constructor must abort before Impl creates its control thread.
  std::span<AudioFatalProbeExpectation const> coreAudioBackendConstructorFatalProbeExpectations() noexcept;
#endif
} // namespace ao::audio::test
