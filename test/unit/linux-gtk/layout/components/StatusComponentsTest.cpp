// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/linux-gtk/playback/NowPlayingStatusLabel.h"
#include "app/linux-gtk/playback/PlaybackDetailsWidget.h"
#include "app/linux-gtk/portal/ImportProgressIndicator.h"
#include "app/linux-gtk/track/LibraryTrackCountLabel.h"
#include "app/linux-gtk/track/StatusNotificationLabel.h"
#include "app/runtime/AppRuntime.h"
#include "app/runtime/ConfigStore.h"
#include "app/runtime/ListSourceStore.h"
#include "runtime/CorePrimitives.h"
#include "test/unit/lmdb/TestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/stack.h>

#include <functional>
#include <memory>

namespace ao::gtk::test
{
  namespace
  {
    class MockExecutor final : public rt::IControlExecutor
    {
    public:
      bool isCurrent() const noexcept override { return true; }
      void dispatch(std::move_only_function<void()> task) override { task(); }
      void defer(std::move_only_function<void()> task) override { task(); }
    };
  } // namespace

  using namespace ao::lmdb::test;

  TEST_CASE("Status bar components", "[gtk][shell]")
  {
    auto const app = Gtk::Application::create("io.github.aobus.status_test");

    auto const tempDir = TempDir{};
    auto const configStore = std::make_shared<rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

    auto runtime = rt::AppRuntime{rt::AppRuntimeDependencies{
      .executor = std::make_unique<MockExecutor>(), .libraryRoot = tempDir.path(), .configStore = configStore}};

    SECTION("PlaybackDetailsWidget instantiates")
    {
      auto widget = PlaybackDetailsWidget{runtime.playback()};
      auto& gtkWidget = widget.widget();
      auto* box = dynamic_cast<Gtk::Box*>(&gtkWidget);
      REQUIRE(box != nullptr);
    }

    SECTION("NowPlayingStatusLabel instantiates and is empty by default")
    {
      auto label = NowPlayingStatusLabel{runtime.playback()};
      auto* gtkLabel = dynamic_cast<Gtk::Label*>(&label.widget());
      REQUIRE(gtkLabel != nullptr);
      CHECK(gtkLabel->get_text().empty());
    }

    SECTION("portal::ImportProgressIndicator instantiates and is hidden by default")
    {
      auto indicator = portal::ImportProgressIndicator{runtime.mutation()};
      REQUIRE(indicator.widget().get_visible() == false);
    }

    SECTION("LibraryTrackCountLabel instantiates and shows 0 tracks")
    {
      auto label = LibraryTrackCountLabel{runtime.sources().allTracks()};
      auto* gtkLabel = dynamic_cast<Gtk::Label*>(&label.widget());
      REQUIRE(gtkLabel != nullptr);
      CHECK(gtkLabel->get_text() == "0 tracks");
    }

    SECTION("StatusNotificationLabel instantiates")
    {
      auto label = StatusNotificationLabel{runtime.notifications(), runtime.views()};
      REQUIRE(dynamic_cast<Gtk::Stack*>(&label.widget()) != nullptr);
    }
  }
} // namespace ao::gtk::test
