// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputDevicePopover.h"

#include "test/unit/MessageCatalogTestSupport.h"
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
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/scrolledwindow.h>

#include <vector>

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

  TEST_CASE("OutputDevicePopover - constructs the configured scrolled list", "[gtk][unit][playback][output]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();

    auto selector = OutputDevicePopover{
      playback, ao::test::englishMessageCatalog(), uimodel::OutputDeviceIntent::discarded(), Gtk::PositionType::BOTTOM};
    drainGtkEvents();

    CHECK(selector.get_autohide());
    CHECK(selector.get_position() == Gtk::PositionType::BOTTOM);

    auto* const listBox = listBoxFor(selector);
    REQUIRE(listBox != nullptr);
    CHECK(listBox->get_selection_mode() == Gtk::SelectionMode::NONE);
    CHECK(hasCssClass(*listBox, "ao-rich-list"));
  }

  TEST_CASE("OutputDevicePopover - activates the rendered exclusive profile and records its selection",
            "[gtk][integration][playback][output]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    rt::test::addReadyAudioProvider(fixture.runtime(), rt::test::makePipeWireOutputStatus());
    drainGtkEvents();
    auto const initial = playback.snapshot().transport.output.selectedDevice;
    REQUIRE(initial.backendId == audio::BackendId{"pipewire"});
    REQUIRE(initial.deviceId == audio::DeviceId{"device1"});
    REQUIRE(initial.profileId == audio::kProfileShared);

    auto selections = std::vector<audio::OutputDeviceSelection>{};
    auto selector = OutputDevicePopover{
      playback,
      ao::test::englishMessageCatalog(),
      uimodel::OutputDeviceIntent::recordedBy([&](auto const& selection) { selections.push_back(selection); }),
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
    auto* const headerRow = listBox->get_row_at_index(0);
    REQUIRE(headerRow != nullptr);
    auto* const header = dynamic_cast<Gtk::Label*>(headerRow->get_child());
    REQUIRE(header != nullptr);
    CHECK(header->get_text() == "PipeWire");
    CHECK(header->has_css_class("ao-menu-header"));
    auto* const sharedRow = listBox->get_row_at_index(1);
    REQUIRE(sharedRow != nullptr);
    REQUIRE(sharedRow->get_child() != nullptr);
    CHECK(sharedRow->get_child()->has_css_class("ao-output-device-selected-row"));
    auto* const exclusiveRow = listBox->get_row_at_index(2);
    REQUIRE(exclusiveRow != nullptr);
    auto* const exclusive = dynamic_cast<Gtk::Box*>(exclusiveRow->get_child());
    REQUIRE(exclusive != nullptr);
    CHECK_FALSE(exclusive->has_css_class("ao-output-device-selected-row"));
    auto const labels = collectAll<Gtk::Label>(*exclusive);
    REQUIRE(labels.size() == 3);
    CHECK(labels[0]->get_text() == "Built-in Audio");
    CHECK(labels[1]->get_text() == "Built-in analog stereo");
    CHECK(labels[2]->get_text() == "[E]");
    REQUIRE(selections.empty());

    emitRowActivated(*listBox, *exclusiveRow);

    auto const selected = playback.snapshot().transport.output.selectedDevice;
    CHECK(selected.backendId == audio::BackendId{"pipewire"});
    CHECK(selected.deviceId == audio::DeviceId{"device1"});
    CHECK(selected.profileId == audio::kProfileExclusive);
    REQUIRE(selections.size() == 1);
    CHECK(selections.front() == selected);
  }
} // namespace ao::gtk::test
