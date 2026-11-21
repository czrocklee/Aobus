// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputDevicePopover.h"

#include "app/AppConfigStore.h"
#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkLayoutTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppState.h>
#include <ao/rt/PlaybackState.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>

#include <catch2/catch_test_macros.hpp>
#include <gtk/gtk.h>
#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/switch.h>
#include <pangomm/layout.h>

#include <memory>
#include <string>
#include <utility>
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

    Gtk::ListBoxRow* rowFor(Gtk::Widget& child)
    {
      return dynamic_cast<Gtk::ListBoxRow*>(child.get_ancestor(Gtk::ListBoxRow::get_base_type()));
    }

    Gtk::ListBoxRow& requireRowByLabel(Gtk::ListBox& list, std::string const& title)
    {
      auto* const label = findLabelByText(list, title);
      REQUIRE(label != nullptr);
      auto* const row = rowFor(*label);
      REQUIRE(row != nullptr);
      return *row;
    }

    Gtk::Switch& requireRoutingPolicySwitch(OutputDevicePopover& selector)
    {
      auto* const toggle = findWidget<Gtk::Switch>(selector);
      REQUIRE(toggle != nullptr);
      return *toggle;
    }
  } // namespace

  TEST_CASE("OutputDevicePopover - constructs the configured scrolled list")
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

  TEST_CASE("OutputDevicePopover - retirement stops observation and rejects retained widget events")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixturePtr = std::make_unique<GtkRuntimeFixture>();
    auto& playback = fixturePtr->runtime().playback();
    rt::test::addReadyAudioProvider(fixturePtr->runtime(), rt::test::makePipeWireOutputStatus());
    drainGtkEvents();

    auto selections = std::vector<audio::OutputDeviceSelection>{};
    auto selector = OutputDevicePopover{
      playback,
      ao::test::englishMessageCatalog(),
      uimodel::OutputDeviceIntent::recordedBy([&](auto const& selection) { selections.push_back(selection); })};
    auto host = GtkWindowFixture{};
    auto button = Gtk::MenuButton{};
    button.set_popover(selector);
    host.mount(button);
    host.present();
    emitShow(selector);
    auto* const listBox = listBoxFor(selector);
    REQUIRE(listBox != nullptr);
    auto& sharedRow = requireRowByLabel(*listBox, "Built-in Audio");
    REQUIRE(sharedRow.get_child() != nullptr);
    REQUIRE(sharedRow.get_child()->has_css_class("ao-output-device-selected-row"));
    auto& toggle = requireRoutingPolicySwitch(selector);
    REQUIRE_FALSE(toggle.get_active());
    toggle.set_active(true); // Queue a native state request, then retire before its idle handoff.
    REQUIRE(selections.empty());

    selector.retire();
    selector.retire();
    playback.commands().setOutputDevice(
      audio::BackendId{"pipewire"}, audio::DeviceId{"node:device1"}, audio::kProfileExclusive);
    drainGtkEvents();
    emitShow(selector);

    REQUIRE(sharedRow.get_child() != nullptr);
    CHECK(sharedRow.get_child()->has_css_class("ao-output-device-selected-row"));
    emitRowActivated(*listBox, sharedRow);
    CHECK(playback.snapshot().transport.output.selectedDevice.profileId == audio::kProfileExclusive);
    REQUIRE(selections.empty());

    // A retired selector may outlive its former service until final GTK
    // destruction. Neither retained event may dereference that service.
    fixturePtr.reset();
    emitShow(selector);
    emitRowActivated(*listBox, sharedRow);
    REQUIRE(toggle.activate());
    drainGtkEvents();
    CHECK(selections.empty());
  }

  TEST_CASE("OutputDevicePopover - pending advisory cannot select the previously configured route")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    rt::test::addReadyAudioProvider(fixture.runtime(), rt::test::makePipeWireOutputStatus());
    drainGtkEvents();
    auto const missing = audio::DeviceId{"node:missing-dac"};
    playback.commands().setOutputDevice(audio::kBackendPipeWire, missing, audio::kProfileShared);

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
    auto* const list = listBoxFor(selector);
    REQUIRE(list != nullptr);
    auto* const advisory = list->get_row_at_index(0);
    REQUIRE(advisory != nullptr);
    auto* const label = dynamic_cast<Gtk::Label*>(advisory->get_child());
    REQUIRE(label != nullptr);
    CHECK(label->get_text() == "Requested output unavailable");
    CHECK_FALSE(advisory->get_activatable());
    emitRowActivated(*list, *advisory);
    CHECK(selections.empty());
    REQUIRE(playback.snapshot().transport.output.optPendingOutputSelection);
    CHECK(playback.snapshot().transport.output.optPendingOutputSelection->deviceId == missing);
  }

  TEST_CASE("OutputDevicePopover - activates the rendered exclusive profile and records its selection")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    rt::test::addReadyAudioProvider(fixture.runtime(), rt::test::makePipeWireOutputStatus());
    drainGtkEvents();
    auto const initial = playback.snapshot().transport.output.selectedDevice;
    REQUIRE(initial.backendId == audio::BackendId{"pipewire"});
    REQUIRE(initial.deviceId == audio::DeviceId{"node:device1"});
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
    auto& headerRow = requireRowByLabel(*listBox, "PipeWire");
    REQUIRE(headerRow.get_child() != nullptr);
    CHECK(headerRow.get_child()->has_css_class("ao-menu-header"));
    auto& sharedRow = requireRowByLabel(*listBox, "Built-in Audio");
    REQUIRE(sharedRow.get_child() != nullptr);
    CHECK(sharedRow.get_child()->has_css_class("ao-output-device-selected-row"));
    auto& exclusiveRow = requireRowByLabel(*listBox, "[E]");
    auto* const exclusive = dynamic_cast<Gtk::Box*>(exclusiveRow.get_child());
    REQUIRE(exclusive != nullptr);
    CHECK_FALSE(exclusive->has_css_class("ao-output-device-selected-row"));
    auto const labels = collectAll<Gtk::Label>(*exclusive);
    REQUIRE(labels.size() == 3);
    CHECK(labels[0]->get_text() == "Built-in Audio");
    CHECK(labels[1]->get_text() == "Built-in analog stereo");
    CHECK(labels[2]->get_text() == "[E]");
    REQUIRE(selections.empty());

    emitRowActivated(*listBox, exclusiveRow);

    auto const selected = playback.snapshot().transport.output.selectedDevice;
    CHECK(selected.backendId == audio::BackendId{"pipewire"});
    CHECK(selected.deviceId == audio::DeviceId{"node:device1"});
    CHECK(selected.profileId == audio::kProfileExclusive);
    REQUIRE(selections.size() == 1);
    CHECK(selections.front() == selected);
  }

  TEST_CASE("OutputDevicePopover - one switch activation records one routing consent change")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    rt::test::addReadyAudioProvider(fixture.runtime(), rt::test::makePipeWireOutputStatus());
    drainGtkEvents();

    auto const fixed = playback.snapshot().transport.output.selectedDevice;
    REQUIRE(fixed.backendId == audio::kBackendPipeWire);
    REQUIRE(fixed.deviceId == audio::DeviceId{"node:device1"});
    REQUIRE(fixed.routingPolicy == audio::OutputRoutingPolicy::Fixed);
    auto allowed = fixed;
    allowed.routingPolicy = audio::OutputRoutingPolicy::AllowSystemReroute;

    auto selections = std::vector<audio::OutputDeviceSelection>{};
    auto const& catalog = ao::test::englishMessageCatalog();
    auto selector = OutputDevicePopover{
      playback,
      catalog,
      uimodel::OutputDeviceIntent::recordedBy([&](auto const& selection) { selections.push_back(selection); })};
    auto host = GtkWindowFixture{};
    auto button = Gtk::MenuButton{};
    button.set_popover(selector);
    host.mount(button);
    host.present();
    emitShow(selector);
    auto* const list = listBoxFor(selector);
    REQUIRE(list != nullptr);
    auto& off = requireRoutingPolicySwitch(selector);
    CHECK_FALSE(off.get_active());
    CHECK_FALSE(off.get_state());
    auto* const row = rowFor(off);
    REQUIRE(row != nullptr);
    CHECK_FALSE(row->get_activatable());
    auto const title = std::string{i18n::requiredText(catalog, i18n::MessageId::OutputDeviceRoutingPolicyToggle)};
    CHECK(hasAccessibleLabel(off, title));
    auto const fixedDescription =
      std::string{i18n::requiredText(catalog, i18n::MessageId::OutputDeviceRoutingPolicyFixed)};
    CHECK(findLabelByText(*row, fixedDescription) != nullptr);

    emitRowActivated(*list, *row);
    drainGtkEvents();
    REQUIRE(selections.empty());

    REQUIRE(off.activate());
    REQUIRE(tryPumpGtkEventsUntil([&] { return !selections.empty(); }));
    REQUIRE(selections == std::vector{allowed});
    CHECK(playback.snapshot().transport.output.selectedDevice == allowed);

    emitShow(selector);
    drainGtkEvents();
    CHECK(selections == std::vector{allowed});
    auto& on = requireRoutingPolicySwitch(selector);
    CHECK(on.get_active());
    CHECK(on.get_state());
    auto* const allowedRow = rowFor(on);
    REQUIRE(allowedRow != nullptr);
    auto const allowedDescription =
      std::string{i18n::requiredText(catalog, i18n::MessageId::OutputDeviceRoutingPolicyAllowed)};
    auto* const description = findLabelByText(*allowedRow, allowedDescription);
    REQUIRE(description != nullptr);

    // The full note wraps instead of being clipped or ellipsized when the chooser is narrow.
    auto* const titleLabel = findLabelByText(*allowedRow, title);
    REQUIRE(titleLabel != nullptr);

    for (auto const* label : {titleLabel, description})
    {
      CHECK(label->get_wrap());
      CHECK(label->get_ellipsize() == Pango::EllipsizeMode::NONE);
    }

    // The explanation must wrap at the chooser's natural desktop width, too.
    CHECK(measureWidget(*allowedRow, Gtk::Orientation::HORIZONTAL, -1).natural <= 400);
    auto const normal = measureWidget(*allowedRow, Gtk::Orientation::VERTICAL, 360);
    auto const narrow = measureWidget(*allowedRow, Gtk::Orientation::VERTICAL, 240);
    CHECK(narrow.minimum > normal.minimum);

    REQUIRE(on.activate());
    REQUIRE(tryPumpGtkEventsUntil([&] { return selections.size() >= 2; }));
    CHECK(selections == (std::vector{allowed, fixed}));
    CHECK(playback.snapshot().transport.output.selectedDevice == fixed);
    CHECK_FALSE(requireRoutingPolicySwitch(selector).get_active());
  }

  TEST_CASE("OutputDevicePopover - queued routing actions respect the current request and latest input")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    rt::test::addReadyAudioProvider(fixture.runtime(), rt::test::makePipeWireOutputStatus());
    drainGtkEvents();
    auto const fixed = playback.snapshot().transport.output.selectedDevice;
    REQUIRE(fixed.routingPolicy == audio::OutputRoutingPolicy::Fixed);

    auto const configPath = fixture.tempDir().path() / "output-picker.yaml";
    auto const storePtr = std::make_shared<AppConfigStore>(configPath);
    auto const recorder = preferredOutputDeviceRecorder(storePtr);
    recorder.record(fixed);
    auto const savedPreference = [&]
    {
      auto prefs = rt::AppPrefsState{};
      AppConfigStore{configPath}.loadAppPrefs(prefs);
      return prefs.preferredOutputSelection;
    };
    REQUIRE(savedPreference() == fixed);

    auto selections = std::vector<audio::OutputDeviceSelection>{};
    auto selector = OutputDevicePopover{playback,
                                        ao::test::englishMessageCatalog(),
                                        uimodel::OutputDeviceIntent::recordedBy(
                                          [&](auto const& selection)
                                          {
                                            selections.push_back(selection);
                                            recorder.record(selection);
                                          })};
    auto host = GtkWindowFixture{};
    auto button = Gtk::MenuButton{};
    button.set_popover(selector);
    host.mount(button);
    host.present();
    emitShow(selector);
    auto& oldToggle = requireRoutingPolicySwitch(selector);
    auto const heldNativePtr = std::unique_ptr<GObject, decltype(&::g_object_unref)>{
      G_OBJECT(g_object_ref(oldToggle.gobj())), &::g_object_unref};

    // A refresh replaces the native row without changing its expected selection.
    // A held switch must no longer be able to grant consent even in that case.
    emitShow(selector);
    ::gtk_switch_set_active(GTK_SWITCH(heldNativePtr.get()), TRUE);
    drainGtkEvents();
    REQUIRE(selections.empty());

    auto& current = requireRoutingPolicySwitch(selector);
    REQUIRE_FALSE(current.get_active());
    current.set_active(true); // Deliver native state-set now, without pumping the idle handoff.
    REQUIRE(selections.empty());

    SECTION("A rebuilt switch for the same request retains the queued action")
    {
      emitShow(selector);
      auto& rebuilt = requireRoutingPolicySwitch(selector);
      CHECK(rebuilt.get_active());
      CHECK_FALSE(rebuilt.get_state());

      SECTION("The action completes once")
      {
        drainGtkEvents();
        auto allowed = fixed;
        allowed.routingPolicy = audio::OutputRoutingPolicy::AllowSystemReroute;
        CHECK(selections == std::vector{allowed});
        CHECK(savedPreference() == allowed);
        CHECK(playback.snapshot().transport.output.selectedDevice == allowed);
        auto& settled = requireRoutingPolicySwitch(selector);
        CHECK(settled.get_active());
        CHECK(settled.get_state());
      }

      SECTION("Returning the thumb to the committed value cancels the action")
      {
        rebuilt.set_active(false);
        drainGtkEvents();
        CHECK(selections.empty());
        CHECK(savedPreference() == fixed);
        CHECK(playback.snapshot().transport.output.selectedDevice == fixed);
        auto& settled = requireRoutingPolicySwitch(selector);
        CHECK_FALSE(settled.get_active());
        CHECK_FALSE(settled.get_state());
      }
    }

    SECTION("A different requested output replaces the row before idle")
    {
      auto const replacementRequest = audio::OutputDeviceSelection{.backendId = audio::kBackendPipeWire,
                                                                   .deviceId = audio::DeviceId{"node:other-dac"},
                                                                   .profileId = audio::kProfileExclusive};
      playback.commands().setOutputDevice(replacementRequest);
      REQUIRE(playback.snapshot().transport.output.optPendingOutputSelection == replacementRequest);
      auto& replacement = requireRoutingPolicySwitch(selector);
      REQUIRE_FALSE(replacement.get_active());
      REQUIRE_FALSE(replacement.get_state());

      SECTION("Without fresh input, the obsolete action cannot change consent")
      {
        drainGtkEvents();
        CHECK(selections.empty());
        CHECK(savedPreference() == fixed);
        CHECK(playback.snapshot().transport.output.optPendingOutputSelection == replacementRequest);
        auto& settled = requireRoutingPolicySwitch(selector);
        CHECK_FALSE(settled.get_active());
        CHECK_FALSE(settled.get_state());
      }

      SECTION("Fresh input on the replacement before idle is recorded and rendered")
      {
        replacement.set_active(true);
        REQUIRE(selections.empty());
        drainGtkEvents();
        auto allowed = replacementRequest;
        allowed.routingPolicy = audio::OutputRoutingPolicy::AllowSystemReroute;
        CHECK(selections == std::vector{allowed});
        CHECK(savedPreference() == allowed);
        CHECK(playback.snapshot().transport.output.selectedDevice == allowed);
        auto& settled = requireRoutingPolicySwitch(selector);
        CHECK(settled.get_active());
        CHECK(settled.get_state());
      }
    }
  }

  TEST_CASE("OutputDevicePopover - omits routing consent for non-PipeWire and system-default routes")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();
    auto selector =
      OutputDevicePopover{playback, ao::test::englishMessageCatalog(), uimodel::OutputDeviceIntent::discarded()};

    SECTION("Non-PipeWire device")
    {
      rt::test::addReadyAudioProvider(fixture.runtime());
      drainGtkEvents();
      REQUIRE(playback.snapshot().transport.output.selectedDevice.backendId == audio::BackendId{"test_backend"});
    }

    SECTION("PipeWire System Default")
    {
      auto status = rt::test::makePipeWireOutputStatus();
      status.devices.push_back(audio::Device{.isDefault = true, .backendId = audio::kBackendPipeWire});
      rt::test::addReadyAudioProvider(fixture.runtime(), std::move(status));
      auto const systemDefault =
        audio::OutputDeviceSelection{.backendId = audio::kBackendPipeWire, .profileId = audio::kProfileShared};
      playback.commands().setOutputDevice(systemDefault);
      drainGtkEvents();
      REQUIRE(playback.snapshot().transport.output.selectedDevice == systemDefault);
    }

    auto host = GtkWindowFixture{};
    auto button = Gtk::MenuButton{};
    button.set_popover(selector);
    host.mount(button);
    host.present();
    emitShow(selector);
    CHECK(findWidget<Gtk::Switch>(selector) == nullptr);

    if (playback.snapshot().transport.output.selectedDevice.deviceId.empty())
    {
      auto* const name = findLabelByText(
        selector,
        std::string{i18n::requiredText(ao::test::englishMessageCatalog(), i18n::MessageId::SystemDefaultOutputDevice)});
      REQUIRE(name != nullptr);
      CHECK(name->get_wrap());
      CHECK(name->get_ellipsize() == Pango::EllipsizeMode::NONE);
    }
  }
} // namespace ao::gtk::test
