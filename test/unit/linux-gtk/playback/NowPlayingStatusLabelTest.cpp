// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/NowPlayingStatusLabel.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/Backend.h>
#include <ao/audio/Types.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Subscription.h>
#include <ao/rt/PlaybackService.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/label.h>

#include <memory>
#include <string_view>

using namespace ao;
using namespace ao::gtk;
using namespace ao::gtk::test;

namespace
{
  class DummyAudioProvider final : public audio::IBackendProvider
  {
  public:
    audio::Subscription subscribeDevices(OnDevicesChangedCallback /*callback*/) override { return {}; }
    std::unique_ptr<audio::IBackend> createBackend(audio::Device const& /*device*/, audio::ProfileId const& /*profileId*/) override
    {
      return std::make_unique<audio::NullBackend>();
    }
    Status status() const override { return Status{.metadata = {.id = audio::kBackendPipeWire}}; }
    audio::Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback /*callback*/) override { return {}; }
  };
}

TEST_CASE("NowPlayingStatusLabel - smoke test", "[gtk][playback][viewmodel]")
{
  [[maybe_unused]] auto const app = ensureGtkApplication();
  auto fixture = GtkRuntimeFixture{};

  fixture.runtime().addAudioProvider(std::make_unique<DummyAudioProvider>());

  auto& playback = fixture.runtime().playback();

  auto statusLabel = NowPlayingStatusLabel{playback};
  auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&statusLabel.widget());
  REQUIRE(gtkLabel);

  // Just verify it wires up and doesn't crash
  auto desc =
    audio::TrackPlaybackDescriptor{.trackId = TrackId{1}, .title = "Song", .artist = "Artist", .durationMs = 1000};

  playback.play(desc, ListId{1});
  drainGtkEvents();
}
