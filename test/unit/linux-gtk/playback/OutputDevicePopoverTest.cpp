// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputDevicePopover.h"

#include "test/unit/PresentationTextCatalogTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>

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
    Gtk::ListBox* listBoxFor(OutputDevicePopover& selector)
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

  TEST_CASE("OutputDevicePopover - renders devices and routes selected output changes", "[gtk][unit][playback][output]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();

    SECTION("constructor wires up the popover with a scrolled list box")
    {
      auto selector = OutputDevicePopover{playback,
                                          ao::test::englishPresentationTextCatalog(),
                                          uimodel::OutputDeviceIntent::discarded(),
                                          Gtk::PositionType::BOTTOM};
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

    SECTION("row activation reports the requested selection")
    {
      rt::test::addReadyAudioProvider(fixture.runtime(), rt::test::makePipeWireOutputStatus());

      auto optSelected = std::optional<audio::OutputDeviceSelection>{};
      auto selector = OutputDevicePopover{
        playback,
        ao::test::englishPresentationTextCatalog(),
        uimodel::OutputDeviceIntent::recordedBy([&optSelected](auto const& selection) { optSelected = selection; }),
        Gtk::PositionType::BOTTOM};
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

      emitRowActivated(*listBox, *exclusiveRow);

      auto const selected = playback.snapshot().transport.output.selectedDevice;
      CHECK(selected.backendId == audio::BackendId{"pipewire"});
      CHECK(selected.deviceId == audio::DeviceId{"device1"});
      CHECK(selected.profileId == audio::kProfileExclusive);
      REQUIRE(optSelected);
      CHECK(*optSelected == selected);
    }
  }
} // namespace ao::gtk::test
