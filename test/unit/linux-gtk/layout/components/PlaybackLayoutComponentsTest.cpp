// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/widget.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("PlaybackLayoutComponents - render idle GTK widgets", "[gtk][unit][layout-component][playback]")
  {
    auto fixture = LayoutRuntimeFixture{};

    SECTION("playPauseButton creates Gtk::Button")
    {
      auto const node = LayoutNode{.type = "playback.playPauseButton"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "media-playback-start-symbolic");
      CHECK(btn->get_sensitive() == false);
      CHECK(btn->has_css_class("ao-playback-button"));
    }

    SECTION("stopButton creates Gtk::Button, insensitive when idle")
    {
      auto const node = LayoutNode{.type = "playback.stopButton"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "media-playback-stop-symbolic");
      CHECK(btn->get_sensitive() == false);
      CHECK(btn->has_css_class("ao-playback-button"));
    }

    SECTION("playButton creates Gtk::Button, insensitive when not ready")
    {
      auto const node = LayoutNode{.type = "playback.playButton"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "media-playback-start-symbolic");
      CHECK(btn->get_sensitive() == false);
      CHECK(btn->has_css_class("ao-playback-button"));
    }

    SECTION("pauseButton creates Gtk::Button, insensitive when not playing")
    {
      auto const node = LayoutNode{.type = "playback.pauseButton"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "media-playback-pause-symbolic");
      CHECK(btn->get_sensitive() == false);
      CHECK(btn->has_css_class("ao-playback-button"));
    }

    SECTION("seekSlider creates Gtk::Scale, insensitive when idle")
    {
      auto const node = LayoutNode{.type = "playback.seekSlider"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const scale = dynamic_cast<Gtk::Scale*>(&compPtr->widget());
      REQUIRE(scale != nullptr);
      CHECK(scale->get_sensitive() == false);
      CHECK(scale->get_value() == 0.0);
      CHECK(scale->has_css_class("ao-seekbar"));
    }

    SECTION("timeLabel creates Gtk::Label with default text")
    {
      auto const node = LayoutNode{.type = "playback.timeLabel"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "00:00 / 00:00");
    }

    SECTION("currentTitleLabel shows Not Playing when idle")
    {
      auto const node = LayoutNode{.type = "playback.currentTitleLabel"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "Not Playing");
      CHECK(label->has_css_class("ao-playback-title"));
    }

    SECTION("currentArtistLabel shows empty when idle")
    {
      auto const node = LayoutNode{.type = "playback.currentArtistLabel"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const label = dynamic_cast<Gtk::Label*>(&compPtr->widget());
      REQUIRE(label != nullptr);
      CHECK(label->get_text().empty());
      CHECK(label->has_css_class("ao-playback-artist"));
    }

    SECTION("volumeControl shows hidden when volume unavailable")
    {
      auto const node = LayoutNode{.type = "playback.volumeControl"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);
      CHECK(compPtr->widget().get_visible() == false);
    }

    SECTION("qualityIndicator creates AobusSoul widget")
    {
      auto const node = LayoutNode{.type = "playback.qualityIndicator"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto& widget = compPtr->widget();
      CHECK(widget.has_css_class("ao-soul"));

      std::int32_t widgetWidth = -1;
      std::int32_t widgetHeight = -1;
      widget.get_size_request(widgetWidth, widgetHeight);
      CHECK(widgetWidth == -1);
      CHECK(widgetHeight == -1);
    }

    SECTION("soulButton creates Gtk::Button with AobusSoul")
    {
      auto const node = LayoutNode{.type = "playback.soulButton"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      CHECK(button->get_has_frame() == false);
      CHECK(button->has_css_class("ao-soul-button"));

      auto* const soul = button->get_child();
      REQUIRE(soul != nullptr);
      CHECK(soul->has_css_class("ao-soul"));

      std::int32_t soulWidth = -1;
      std::int32_t soulHeight = -1;
      soul->get_size_request(soulWidth, soulHeight);
      CHECK(soulWidth == -1);
      CHECK(soulHeight == -1);

      CHECK(soul->get_hexpand() == false);
      CHECK(soul->get_vexpand() == false);
      CHECK(soul->get_halign() == Gtk::Align::FILL);
      CHECK(soul->get_valign() == Gtk::Align::FILL);
    }

    SECTION("outputDeviceSelector creates Gtk::Button with Label")
    {
      auto const node = LayoutNode{.type = "playback.outputDeviceSelector"};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      CHECK(button->get_has_frame() == false);
      CHECK(button->has_css_class("ao-output-device-selector-modern"));

      auto* const label = dynamic_cast<Gtk::Label*>(button->get_child());
      REQUIRE(label != nullptr);
      CHECK(label->get_text() == "--"); // Default backend summary
    }

    SECTION("all 13 playback types register and instantiate")
    {
      auto const types = std::to_array<std::string_view>({"playback.playPauseButton",
                                                          "playback.stopButton",
                                                          "playback.volumeControl",
                                                          "playback.currentTitleLabel",
                                                          "playback.currentArtistLabel",
                                                          "playback.seekSlider",
                                                          "playback.timeLabel",
                                                          "playback.playButton",
                                                          "playback.pauseButton",
                                                          "playback.qualityIndicator",
                                                          "playback.soulPlayPauseButton",
                                                          "playback.soulButton",
                                                          "playback.outputDeviceSelector"});

      for (auto const type : types)
      {
        auto const node = LayoutNode{.type = std::string{type}};
        auto const compPtr = fixture.create(node);
        CHECK(compPtr != nullptr);
      }
    }
  }
} // namespace ao::gtk::layout::test
