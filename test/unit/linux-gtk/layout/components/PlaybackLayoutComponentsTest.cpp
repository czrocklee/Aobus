// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/AobusSoul.h"
#include "app/linux-gtk/layout/component/ComponentRegistrations.h"
#include "app/linux-gtk/layout/runtime/LayoutComponent.h"
#include "app/linux-gtk/playback/OutputDevicePopover.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/linux-gtk/layout/LayoutTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scale.h>
#include <gtkmm/widget.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::gtk::layout::test
{
  using namespace uimodel;

  TEST_CASE("PlaybackLayoutComponents - render idle GTK widgets", "[gtk][unit][layout-component][playback]")
  {
    auto fixture = LayoutRuntimeFixture{};

    SECTION("transportButton playPause creates Gtk::Button")
    {
      auto const node =
        LayoutNode{.type = "playback.transportButton", .props = {{"command", LayoutValue{"playPause"}}}};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "media-playback-start-symbolic");
      CHECK(btn->get_sensitive() == false);
      CHECK(btn->has_css_class("ao-playback-button"));
    }

    SECTION("transportButton stop creates Gtk::Button, insensitive when idle")
    {
      auto const node = LayoutNode{.type = "playback.transportButton", .props = {{"command", LayoutValue{"stop"}}}};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "media-playback-stop-symbolic");
      CHECK(btn->get_sensitive() == false);
      CHECK(btn->has_css_class("ao-playback-button"));
    }

    SECTION("transportButton play creates Gtk::Button, insensitive when not ready")
    {
      auto const node = LayoutNode{.type = "playback.transportButton", .props = {{"command", LayoutValue{"play"}}}};
      auto const compPtr = fixture.create(node);

      REQUIRE(compPtr != nullptr);

      auto* const btn = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(btn != nullptr);
      CHECK(btn->get_icon_name() == "media-playback-start-symbolic");
      CHECK(btn->get_sensitive() == false);
      CHECK(btn->has_css_class("ao-playback-button"));
    }

    SECTION("transportButton pause creates Gtk::Button, insensitive when not playing")
    {
      auto const node = LayoutNode{.type = "playback.transportButton", .props = {{"command", LayoutValue{"pause"}}}};
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
      auto windowFixture = ao::gtk::test::GtkWindowFixture{};
      windowFixture.mount(compPtr->widget());
      windowFixture.present();
      CHECK(button->get_has_frame() == false);
      CHECK(button->has_css_class("ao-soul-button"));
      CHECK(ao::gtk::test::hasAccessibleLabel(*button, "Aobus Soul"));

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

    SECTION("Soul components apply custom stroke and glyph scale properties")
    {
      for (auto const* const type : {"playback.soulButton", "playback.soulPlayPauseButton"})
      {
        auto node = LayoutNode{.type = type};
        node.props["strokeWidth"] = LayoutValue{5.0};
        node.props["glyphScale"] = LayoutValue{0.85};
        auto const compPtr = fixture.create(node);

        REQUIRE(compPtr != nullptr);
        auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
        REQUIRE(button != nullptr);
        auto* const soul = dynamic_cast<AobusSoul*>(button->get_child());
        REQUIRE(soul != nullptr);
        CHECK(soul->baseStrokeWidth() == 5.0F);
        CHECK(soul->innerGlyphScale() == 0.85F);
      }
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

    SECTION("outputDeviceSelector reports the exact route selected from its popover")
    {
      rt::test::addReadyAudioProvider(fixture.runtime(), rt::test::makePipeWireOutputStatus());
      auto optRequested = std::optional<audio::OutputDeviceSelection>{};
      registerOutputDeviceSelectorComponent(
        fixture.components(),
        fixture.runtime().playback(),
        ao::test::messageCatalog("en"),
        uimodel::OutputDeviceIntent::recordedBy([&optRequested](audio::OutputDeviceSelection const& selection)
                                                { optRequested = selection; }));
      auto const node = LayoutNode{.type = "playback.outputDeviceSelector"};
      auto const compPtr = fixture.create(node);
      REQUIRE(compPtr != nullptr);
      auto* const button = dynamic_cast<Gtk::Button*>(&compPtr->widget());
      REQUIRE(button != nullptr);
      auto host = ao::gtk::test::GtkWindowFixture{};
      host.mount(*button);
      host.present();

      ao::gtk::test::emitClicked(*button);
      ao::gtk::test::drainGtkEvents();
      auto* const popover = ao::gtk::test::findWidget<OutputDevicePopover>(*button);
      REQUIRE(popover != nullptr);
      ao::gtk::test::emitShow(*popover);
      ao::gtk::test::drainGtkEvents();
      auto* const listBox = ao::gtk::test::findWidget<Gtk::ListBox>(*popover);
      REQUIRE(listBox != nullptr);
      auto* const exclusiveRow = listBox->get_row_at_index(2);
      REQUIRE(exclusiveRow != nullptr);

      ao::gtk::test::emitRowActivated(*listBox, *exclusiveRow);

      REQUIRE(optRequested);
      CHECK(optRequested->backendId == audio::BackendId{"pipewire"});
      CHECK(optRequested->deviceId == audio::DeviceId{"device1"});
      CHECK(optRequested->profileId == audio::kProfileExclusive);
    }

    SECTION("all 10 playback types register and instantiate")
    {
      auto const types = std::to_array<std::string_view>({"playback.transportButton",
                                                          "playback.volumeControl",
                                                          "playback.currentTitleLabel",
                                                          "playback.currentArtistLabel",
                                                          "playback.seekSlider",
                                                          "playback.timeLabel",
                                                          "playback.qualityIndicator",
                                                          "playback.soulPlayPauseButton",
                                                          "playback.soulButton",
                                                          "playback.outputDeviceSelector"});

      for (auto const type : types)
      {
        INFO(type);
        auto const node = LayoutNode{.type = std::string{type}};
        auto const compPtr = fixture.create(node);
        REQUIRE(compPtr != nullptr);
        // A registry answers an unknown type with a placeholder rather than
        // nullptr, so a non-null component alone does not mean it registered.
        CHECK_FALSE(containsLayoutErrorPlaceholder(compPtr->widget()));
      }
    }
  }
} // namespace ao::gtk::layout::test
