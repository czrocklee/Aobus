// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "platform/AudioBackendBootstrap.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/Backend.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

namespace ao::gtk::test
{
  namespace
  {
    bool hasBackend(rt::PlaybackState const& state, audio::BackendId const& id)
    {
      return std::ranges::any_of(
        state.availableOutputs, [&id](rt::OutputBackendSnapshot const& backend) { return backend.id == id; });
    }
  } // namespace

  // The provider metadata is hardcoded, so each compiled-in backend surfaces in
  // availableOutputs regardless of whether a daemon or hardware is present. The
  // PIPEWIRE_FOUND/ALSA_FOUND macros reach this TU through ao_audio's PUBLIC
  // definitions.
  TEST_CASE("registerPlatformAudioBackends - registers the compiled-in backends", "[gtk][platform][audio]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();

    REQUIRE(playback.state().availableOutputs.empty());

    registerPlatformAudioBackends(fixture.runtime());
    drainGtkEvents();

#ifdef PIPEWIRE_FOUND
    CHECK(hasBackend(playback.state(), audio::kBackendPipeWire));
#endif
#ifdef ALSA_FOUND
    CHECK(hasBackend(playback.state(), audio::kBackendAlsa));
#endif
  }
} // namespace ao::gtk::test
