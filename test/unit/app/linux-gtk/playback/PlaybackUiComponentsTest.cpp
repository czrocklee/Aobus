// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "../../../../unit/lmdb/TestUtils.h"
#include "ao/audio/Types.h"
#include "playback/SeekControl.h"
#include "playback/TimeLabel.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/PlaybackService.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/adjustment.h>  // NOLINT(misc-include-cleaner)
#include <gtkmm/application.h> // NOLINT(misc-include-cleaner)
#include <gtkmm/label.h>
#include <gtkmm/scale.h>

#include <functional>
#include <memory>
#include <vector>

namespace ao::gtk::test
{
  using namespace ao::lmdb::test;

  namespace
  {
    class MockExecutor final : public rt::IControlExecutor
    {
    public:
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); }
      void defer(std::move_only_function<void()> task) override { task(); }
    };

    struct TestEnvironment final
    {
      TempDir tempDir{};
      std::shared_ptr<rt::ConfigStore> configStore;
      rt::AppRuntime runtime;

      TestEnvironment()
        : configStore{std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml")}
        , runtime{
            rt::AppRuntimeDependencies{.executor = std::make_unique<MockExecutor>(),
                                       .musicRoot = tempDir.path(),
                                       .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
                                       .workspaceConfigStore = configStore}}
      {
      }
    };
  } // namespace

  class SeekControlTestPeer final
  {
  public:
    static void beginPointerSeek(SeekControl& control) { control.beginUserInteraction(); }
    static void endPointerSeek(SeekControl& control) { control.endUserInteraction(); }
    static void flushPendingSeeks(SeekControl& control) { control.executeDebouncedFinalSeek(); }
  };

  TEST_CASE("Playback UI Components - Integration Behavior", "[playback][unit][ui]")
  {
    // GTK initialization for widget creation
    [[maybe_unused]] auto const app = Gtk::Application::create("io.github.aobus.ui_test");
    auto env = TestEnvironment{};
    auto& playback = env.runtime.playback();

    SECTION("SeekControl and TimeLabel reaction to seek updates")
    {
      auto seekControl = SeekControl{playback};
      auto timeLabel = TimeLabel{playback};

      auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
      auto* const label = dynamic_cast<Gtk::Label*>(&timeLabel.widget());

      REQUIRE(scale != nullptr);
      REQUIRE(label != nullptr);

      // 1. Initial State: Duration 0, Position 0, Disabled
      CHECK(scale->get_sensitive() == false);
      CHECK(label->get_text() == "00:00 / 00:00");

      // 2. Simulate playback start (e.g., 5 min track)
      // Note: We simulate the signal directly via PlaybackService
      // In a real scenario, this happens when a track starts.
      auto const desc = audio::TrackPlaybackDescriptor{.trackId = TrackId{1}, .title = "Test", .durationMs = 300000};
      playback.play(desc, ListId{1});

      CHECK(scale->get_sensitive() == true);
      CHECK(scale->get_adjustment()->get_upper() == 300000.0);
      CHECK(label->get_text() == "0:00 / 5:00");

      // 3. Simulate a Final Seek to 2:30 (150,000 ms)
      playback.seek(150000, rt::PlaybackService::SeekMode::Final);

      CHECK(scale->get_value() == 150000.0);
      CHECK(label->get_text() == "2:30 / 5:00");

      // 4. Simulate a Preview Seek (e.g., while dragging)
      playback.seek(60000, rt::PlaybackService::SeekMode::Preview);

      // Note: Final scale value in SeekControl might not update during dragging
      // but the TimeLabel should reflect the preview position immediately.
      CHECK(label->get_text() == "1:00 / 5:00");
    }

    SECTION("SeekControl commits pointer seek exactly once on release")
    {
      auto seekControl = SeekControl{playback};
      auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());

      REQUIRE(scale != nullptr);

      auto const desc = audio::TrackPlaybackDescriptor{.trackId = TrackId{1}, .title = "Test", .durationMs = 300000};
      playback.play(desc, ListId{1});

      auto seekEvents = std::vector<rt::PlaybackService::SeekUpdate>{};
      [[maybe_unused]] auto seekSub =
        playback.onSeekUpdate([&seekEvents](rt::PlaybackService::SeekUpdate const& ev) { seekEvents.push_back(ev); });

      SeekControlTestPeer::beginPointerSeek(seekControl);
      scale->set_value(120000.0);

      REQUIRE(seekEvents.size() == 1);
      CHECK(seekEvents[0].mode == rt::PlaybackService::SeekMode::Preview);
      CHECK(seekEvents[0].positionMs == 120000);

      SeekControlTestPeer::endPointerSeek(seekControl);
      SeekControlTestPeer::flushPendingSeeks(seekControl);

      REQUIRE(seekEvents.size() == 2);
      CHECK(seekEvents[1].mode == rt::PlaybackService::SeekMode::Final);
      CHECK(seekEvents[1].positionMs == 120000);

      SeekControlTestPeer::endPointerSeek(seekControl);

      CHECK(seekEvents.size() == 2);
    }

    SECTION("SeekControl ignores release after reset")
    {
      auto seekControl = SeekControl{playback};
      auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());

      REQUIRE(scale != nullptr);

      auto const desc = audio::TrackPlaybackDescriptor{.trackId = TrackId{1}, .durationMs = 100000};
      playback.play(desc, ListId{1});

      auto seekEvents = std::vector<rt::PlaybackService::SeekUpdate>{};
      [[maybe_unused]] auto seekSub =
        playback.onSeekUpdate([&seekEvents](rt::PlaybackService::SeekUpdate const& ev) { seekEvents.push_back(ev); });

      SeekControlTestPeer::beginPointerSeek(seekControl);
      scale->set_value(40000.0);

      REQUIRE(seekEvents.size() == 1);
      CHECK(seekEvents[0].mode == rt::PlaybackService::SeekMode::Preview);

      playback.stop();
      SeekControlTestPeer::endPointerSeek(seekControl);

      CHECK(seekEvents.size() == 1);
      CHECK(scale->get_sensitive() == false);
      CHECK(scale->get_value() == 0.0);
    }

    SECTION("SeekControl applies external final seek without emitting another seek")
    {
      auto seekControl = SeekControl{playback};
      auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());

      REQUIRE(scale != nullptr);

      auto const desc = audio::TrackPlaybackDescriptor{.trackId = TrackId{1}, .durationMs = 100000};
      playback.play(desc, ListId{1});

      auto seekEvents = std::vector<rt::PlaybackService::SeekUpdate>{};
      [[maybe_unused]] auto seekSub =
        playback.onSeekUpdate([&seekEvents](rt::PlaybackService::SeekUpdate const& ev) { seekEvents.push_back(ev); });

      playback.seek(45000, rt::PlaybackService::SeekMode::Final);

      REQUIRE(seekEvents.size() == 1);
      CHECK(seekEvents[0].mode == rt::PlaybackService::SeekMode::Final);
      CHECK(seekEvents[0].positionMs == 45000);
      CHECK(scale->get_value() == 45000.0);
    }

    SECTION("Reset on idle/stop")
    {
      auto seekControl = SeekControl{playback};
      auto timeLabel = TimeLabel{playback};

      auto const desc = audio::TrackPlaybackDescriptor{.trackId = TrackId{1}, .durationMs = 100000};
      playback.play(desc, ListId{1});
      playback.stop();

      auto* const scale = dynamic_cast<Gtk::Scale*>(&seekControl.widget());
      auto* const label = dynamic_cast<Gtk::Label*>(&timeLabel.widget());

      CHECK(scale->get_sensitive() == false);
      CHECK(scale->get_value() == 0.0);
      CHECK(label->get_text() == "00:00 / 00:00");
    }

    SECTION("TimeLabel modes (Elapsed and Duration)")
    {
      auto elapsedLabel = TimeLabel{playback, TimeLabelMode::Elapsed};
      auto durationLabel = TimeLabel{playback, TimeLabelMode::Duration};

      auto* const elWidget = dynamic_cast<Gtk::Label*>(&elapsedLabel.widget());
      auto* const durWidget = dynamic_cast<Gtk::Label*>(&durationLabel.widget());

      REQUIRE(elWidget != nullptr);
      REQUIRE(durWidget != nullptr);

      // 1. Initial State
      CHECK(elWidget->get_text() == "00:00");
      CHECK(durWidget->get_text() == "00:00");

      // 2. Simulate playback start (5 min track)
      auto const desc = audio::TrackPlaybackDescriptor{.trackId = TrackId{1}, .durationMs = 300000};
      playback.play(desc, ListId{1});

      CHECK(elWidget->get_text() == "0:00");
      CHECK(durWidget->get_text() == "5:00");

      // 3. Simulate Seek to 2:30
      playback.seek(150000, rt::PlaybackService::SeekMode::Final);
      CHECK(elWidget->get_text() == "2:30");
      CHECK(durWidget->get_text() == "5:00");
    }
  }
} // namespace ao::gtk::test
