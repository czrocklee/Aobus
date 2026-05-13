// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <app/linux-gtk/shell/ImportProgressIndicator.h>
#include <app/linux-gtk/shell/LibraryTrackCountLabel.h>
#include <app/linux-gtk/shell/NowPlayingStatusLabel.h>
#include <app/linux-gtk/shell/PlaybackDetailsWidget.h>
#include <app/linux-gtk/shell/StatusNotificationLabel.h>
#include <app/runtime/AppSession.h>
#include <app/runtime/ConfigStore.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>

#include <test/unit/lmdb/TestUtils.h>

using namespace ao::gtk;

namespace
{
  class MockExecutor final : public ao::rt::IControlExecutor
  {
  public:
    bool isCurrent() const noexcept override { return true; }
    void dispatch(std::move_only_function<void()> task) override { task(); }
    void defer(std::move_only_function<void()> task) override { task(); }
  };
}

TEST_CASE("Status bar components", "[gtk][shell]")
{
  auto const app = Gtk::Application::create("io.github.aobus.status_test");

  auto const tempDir = TempDir{};
  auto const executor = std::make_shared<MockExecutor>();
  auto const configStore = std::make_shared<ao::rt::ConfigStore>(std::filesystem::path{tempDir.path()} / "config.yaml");

  auto session = ao::rt::AppSession{
    ao::rt::AppSessionDependencies{.executor = executor, .libraryRoot = tempDir.path(), .configStore = configStore}};

  SECTION("PlaybackDetailsWidget instantiates")
  {
    auto widget = PlaybackDetailsWidget{session};
    auto& gtkWidget = widget.widget();
    auto* box = dynamic_cast<Gtk::Box*>(&gtkWidget);
    REQUIRE(box != nullptr);
  }

  SECTION("NowPlayingStatusLabel instantiates and is empty by default")
  {
    auto label = NowPlayingStatusLabel{session};
    auto* gtkLabel = dynamic_cast<Gtk::Label*>(&label.widget());
    REQUIRE(gtkLabel != nullptr);
    CHECK(gtkLabel->get_text() == "");
  }

  SECTION("ImportProgressIndicator instantiates and is hidden by default")
  {
    auto indicator = ImportProgressIndicator{session};
    REQUIRE(indicator.widget().get_visible() == false);
  }

  SECTION("LibraryTrackCountLabel instantiates and shows 0 tracks")
  {
    auto label = LibraryTrackCountLabel{session};
    auto* gtkLabel = dynamic_cast<Gtk::Label*>(&label.widget());
    REQUIRE(gtkLabel != nullptr);
    CHECK(gtkLabel->get_text() == "0 tracks");
  }

  SECTION("StatusNotificationLabel instantiates")
  {
    auto label = StatusNotificationLabel{session};
    REQUIRE(dynamic_cast<Gtk::Stack*>(&label.widget()) != nullptr);
  }
}
