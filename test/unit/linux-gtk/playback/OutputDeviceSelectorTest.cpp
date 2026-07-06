// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputDeviceSelector.h"

#include "test/unit/RuntimeTestUtils.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/Backend.h>
#include <ao/rt/PlaybackState.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/enums.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/scrolledwindow.h>

#include <optional>

namespace ao::gtk::test
{
  namespace
  {
    Gtk::ListBox* listBoxFor(OutputDeviceSelector& selector)
    {
      auto* const scrolled = dynamic_cast<Gtk::ScrolledWindow*>(selector.get_child());

      if (scrolled == nullptr)
      {
        return nullptr;
      }

      auto* const viewport = scrolled->get_child();

      if (viewport == nullptr)
      {
        return nullptr;
      }

      return dynamic_cast<Gtk::ListBox*>(viewport->get_first_child());
    }
  } // namespace

  TEST_CASE("OutputDeviceSelector renders devices and routes selected output changes", "[gtk][unit][playback][output]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();

    SECTION("constructor wires up the popover with a scrolled list box")
    {
      auto selector = OutputDeviceSelector{playback, Gtk::PositionType::BOTTOM};
      drainGtkEvents();

      CHECK(selector.get_autohide());
      CHECK(selector.get_position() == Gtk::PositionType::BOTTOM);

      auto* const scrolled = dynamic_cast<Gtk::ScrolledWindow*>(selector.get_child());
      REQUIRE(scrolled != nullptr);

      auto* const viewport = scrolled->get_child();
      REQUIRE(viewport != nullptr);
      auto* const listBox = dynamic_cast<Gtk::ListBox*>(viewport->get_first_child());
      REQUIRE(listBox != nullptr);
      CHECK(listBox->get_selection_mode() == Gtk::SelectionMode::NONE);
      CHECK(hasCssClass(*listBox, "ao-rich-list"));
    }

    SECTION("row activation reports the engine-confirmed selection")
    {
      rt::test::addReadyAudioProvider(playback, rt::test::makePipeWireOutputStatus());

      auto optSelected = std::optional<rt::OutputDeviceSelection>{};
      auto selector = OutputDeviceSelector{
        playback, Gtk::PositionType::BOTTOM, [&optSelected](auto const& selection) { optSelected = selection; }};
      auto host = GtkWindowFixture{};
      auto button = Gtk::MenuButton{};
      button.set_popover(selector);
      host.mount(button);
      host.present();

      emitShow(selector);
      drainGtkEvents();

      auto* const listBox = listBoxFor(selector);
      REQUIRE(listBox != nullptr);
      auto* const exclusiveRow = listBox->get_row_at_index(2);
      REQUIRE(exclusiveRow != nullptr);

      ::g_signal_emit_by_name(listBox->gobj(), "row-activated", exclusiveRow->gobj());

      auto const& selected = playback.state().output.selectedDevice;
      CHECK(selected.backendId == audio::BackendId{"pipewire"});
      CHECK(selected.deviceId == audio::DeviceId{"device1"});
      CHECK(selected.profileId == audio::kProfileExclusive);
      REQUIRE(optSelected.has_value());
      CHECK(*optSelected == selected);
    }
  }
} // namespace ao::gtk::test
