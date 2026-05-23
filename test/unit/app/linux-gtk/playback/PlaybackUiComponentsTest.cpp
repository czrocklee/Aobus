// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/SeekControl.h"
#include "playback/TimeLabel.h"
#include "ao/audio/Types.h"
#include "runtime/AppRuntime.h"
#include "runtime/ConfigStore.h"
#include "runtime/PlaybackService.h"
#include "runtime/CorePrimitives.h"
#include "../../../../unit/lmdb/TestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/adjustment.h> // NOLINT(misc-include-cleaner)
#include <gtkmm/application.h> // NOLINT(misc-include-cleaner)
#include <gtkmm/label.h>
#include <gtkmm/scale.h>

#include <functional>
#include <memory>

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
        : configStore{std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml")},
          runtime{rt::AppRuntimeDependencies{
            .executor = std::make_unique<MockExecutor>(),
            .musicRoot = tempDir.path(),
            .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
            .globalConfigStore = configStore,
            .workspaceConfigStore = configStore}}
      {
      }
    };
  } // namespace

  TEST_CASE("Playback UI Components - Integration Behavior", "[playback][ui]")
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
  }
} // namespace ao::gtk::test
