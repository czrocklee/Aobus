// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/layout/components/StatusComponents.h"

#include "../../GtkTestSupport.h"
#include "app/linux-gtk/layout/runtime/ComponentRegistry.h"
#include "app/linux-gtk/playback/NowPlayingStatusLabel.h"
#include "app/linux-gtk/playback/PlaybackDetailsWidget.h"
#include "app/linux-gtk/track/LibraryTrackCountLabel.h"
#include "app/linux-gtk/track/StatusSlot.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/ListSourceStore.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>

#include <memory>

namespace ao::gtk::layout::test
{
  using ao::gtk::test::ImmediateExecutor;

  namespace
  {
    using namespace ao::lmdb::test;
  } // namespace

  TEST_CASE("Status bar components", "[gtk][unit][shell]")
  {
    auto const app = Gtk::Application::create("io.github.aobus.status_test");

    auto const tempDir = TempDir{};
    auto const configStore = std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto runtime = rt::AppRuntime{
      rt::AppRuntimeDependencies{.executor = std::make_unique<ImmediateExecutor>(),
                                 .musicRoot = tempDir.path(),
                                 .databasePath = std::filesystem::path{tempDir.path()} / ".aobus" / "library",
                                 .workspaceConfigStore = configStore}};

    SECTION("PlaybackDetailsWidget instantiates")
    {
      auto widget = PlaybackDetailsWidget{runtime.playback()};
      auto& gtkWidget = widget.widget();
      auto* const box = dynamic_cast<Gtk::Box*>(&gtkWidget);
      REQUIRE(box != nullptr);
    }

    SECTION("NowPlayingStatusLabel instantiates and is empty by default")
    {
      auto label = NowPlayingStatusLabel{runtime.playback()};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&label.widget());
      REQUIRE(gtkLabel != nullptr);
      CHECK(gtkLabel->get_text().empty());
    }

    SECTION("LibraryTrackCountLabel instantiates and shows 0 tracks")
    {
      auto label = LibraryTrackCountLabel{runtime.sources().allTracks()};
      auto* const gtkLabel = dynamic_cast<Gtk::Label*>(&label.widget());
      REQUIRE(gtkLabel != nullptr);
      CHECK(gtkLabel->get_text() == "0 tracks");
    }

    SECTION("StatusSlot instantiates")
    {
      auto slot = StatusSlot{runtime.mutation(), runtime.notifications(), runtime.views()};
      REQUIRE(dynamic_cast<Gtk::Box*>(&slot.widget()) != nullptr);
    }

    SECTION("StatusComponents Registration")
    {
      auto registry = ComponentRegistry{};
      registerStatusComponents(registry);

      {
        auto const optDesc = registry.descriptor("status.statusSlot");
        REQUIRE(optDesc.has_value());
        CHECK(optDesc->displayName == "Status Slot");
      }
    }
  }
} // namespace ao::gtk::layout::test
