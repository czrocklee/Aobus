// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/input/KeymapAcceleratorPlan.h>

#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/winui/input/KeyChordAccelerator.h>
#include <ao/winui/layout/LayoutCatalog.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::winui::test
{
  namespace
  {
    using uimodel::KeyChord;
    using uimodel::KeymapBindings;
    using uimodel::KeymapModel;
    using uimodel::LayoutActionCatalog;

    KeyChord chord(std::string const& text)
    {
      auto optChord = KeyChord::parse(text);
      REQUIRE(optChord);
      return *optChord;
    }

    LayoutActionCatalog catalogWith(std::string id, uimodel::LayoutActionCapabilities const capabilities)
    {
      auto catalog = LayoutActionCatalog{};
      catalog.registerActionDescriptor(
        {.id = std::move(id), .label = "Test", .category = "Test", .capabilities = capabilities});
      return catalog;
    }

    /// Answers yes for everything, so a case can isolate the other rules.
    KeymapActionAvailability const kEverythingOffered = [](std::string_view) { return true; };

    bool shippedWindowsAction(LayoutActionCatalog const& catalog, std::string_view const id)
    {
      if (catalog.descriptor(id))
      {
        return true;
      }

      // These commands are native shell behavior rather than layout-document
      // actions. ShellBuilder registers their live handlers directly.
      return id == "workspace.revealCurrentTrack" || id == "track.orderMoveUp" || id == "track.orderMoveDown" ||
             id == "track.orderMoveToTop" || id == "track.orderMoveToBottom";
    }
  } // namespace

  TEST_CASE("KeymapAcceleratorPlan - a binding this shell serves becomes an accelerator", "[winui][unit][input]")
  {
    auto const keymap = KeymapModel{KeymapBindings{{"playback.playPause", {chord("Ctrl+P")}}}};

    auto const plans = planKeymapAccelerators(keymap, LayoutActionCatalog{}, kEverythingOffered);

    REQUIRE(plans.size() == 1);
    CHECK(plans.front().actionId == "playback.playPause");
    CHECK(plans.front().key.virtualKey == 0x50);
    CHECK(plans.front().key.modifiers == kAcceleratorModifierControl);
  }

  TEST_CASE("KeymapAcceleratorPlan - an action this shell has no handler for is dropped", "[winui][unit][input]")
  {
    auto const keymap =
      KeymapModel{KeymapBindings{{"playback.playPause", {chord("Ctrl+P")}}, {"track.orderMoveUp", {chord("Alt+Up")}}}};

    auto const plans = planKeymapAccelerators(
      keymap, LayoutActionCatalog{}, [](std::string_view const id) { return id == "playback.playPause"; });

    REQUIRE(plans.size() == 1);
    CHECK(plans.front().actionId == "playback.playPause");
  }

  TEST_CASE("KeymapAcceleratorPlan - an action that presents from an anchor is dropped", "[winui][unit][input]")
  {
    // A keystroke has no anchor, so binding one would raise a menu at nowhere.
    auto const keymap = KeymapModel{KeymapBindings{{"playback.showOutputDeviceSelector", {chord("Ctrl+O")}}}};
    auto const catalog =
      catalogWith("playback.showOutputDeviceSelector",
                  uimodel::LayoutActionCapability::RequiresAnchor | uimodel::LayoutActionCapability::PresentsMenu);

    CHECK(planKeymapAccelerators(keymap, catalog, kEverythingOffered).empty());
  }

  TEST_CASE("KeymapAcceleratorPlan - a menu that needs no anchor is kept", "[winui][unit][input]")
  {
    auto const keymap = KeymapModel{KeymapBindings{{"shell.showSoul", {chord("Ctrl+K")}}}};
    auto const catalog = catalogWith("shell.showSoul", uimodel::LayoutActionCapability::PresentsMenu);

    CHECK(planKeymapAccelerators(keymap, catalog, kEverythingOffered).size() == 1);
  }

  TEST_CASE("KeymapAcceleratorPlan - a chord Windows cannot express is dropped", "[winui][unit][input]")
  {
    auto const keymap =
      KeymapModel{KeymapBindings{{"playback.playPause", {KeyChord{.key = "Hyper"}, chord("Ctrl+P")}}}};

    auto const plans = planKeymapAccelerators(keymap, LayoutActionCatalog{}, kEverythingOffered);

    REQUIRE(plans.size() == 1);
    CHECK(plans.front().key.virtualKey == 0x50);
  }

  TEST_CASE("KeymapAcceleratorPlan - media chords are left to the system media controls", "[winui][unit][input]")
  {
    // The shell registers for system media control, which already runs the
    // command. An accelerator on the same key would run it twice while the
    // window has focus.
    auto const keymap =
      KeymapModel{KeymapBindings{{"playback.playPause", {chord("Media:Play"), chord("Media:Pause"), chord("Ctrl+P")}},
                                 {"playback.stop", {chord("Media:Stop")}}}};

    auto const plans = planKeymapAccelerators(keymap, LayoutActionCatalog{}, kEverythingOffered);

    REQUIRE(plans.size() == 1);
    CHECK(plans.front().actionId == "playback.playPause");
    CHECK(plans.front().key.virtualKey == 0x50);
  }

  TEST_CASE("KeymapAcceleratorPlan - two chords on one Windows key yield one accelerator", "[winui][unit][input]")
  {
    auto const keymap = KeymapModel{KeymapBindings{{"playback.next", {chord("Ctrl+Right"), chord("Ctrl+Right")}}}};

    auto const plans = planKeymapAccelerators(keymap, LayoutActionCatalog{}, kEverythingOffered);

    CHECK(plans.size() == 1);
  }

  TEST_CASE("KeymapAcceleratorPlan - two actions cannot claim one Windows key", "[winui][unit][input]")
  {
    // `Ctrl++` and `Ctrl+Shift+=` read differently and are the same keystroke,
    // because the character `+` carries its own Shift. Installing both would
    // leave which action runs to XAML's ordering, so the first declaration
    // keeps the key.
    auto const keymap = KeymapModel{KeymapBindings{
      {"playback.playPause", {chord("Ctrl++")}},
      {"playback.stop", {chord("Ctrl+Shift+=")}},
    }};

    auto const plans = planKeymapAccelerators(keymap, LayoutActionCatalog{}, kEverythingOffered);

    REQUIRE(plans.size() == 1);
    CHECK(plans.front().actionId == "playback.playPause");
  }

  TEST_CASE("KeymapAcceleratorPlan - one action keeps every distinct key it asks for", "[winui][unit][input]")
  {
    // The conflict rule must not swallow a second chord that reaches a
    // different key, which is the ordinary way an action gets two shortcuts.
    auto const keymap =
      KeymapModel{KeymapBindings{{"playback.next", {chord("Ctrl+Right"), chord("Ctrl+N"), chord("Ctrl+Right")}}}};

    auto const plans = planKeymapAccelerators(keymap, LayoutActionCatalog{}, kEverythingOffered);

    CHECK(plans.size() == 2);
  }

  TEST_CASE("KeymapAcceleratorPlan - the shipped shell reaches its keyboard commands", "[winui][unit][input]")
  {
    // Layout actions and native-only shell commands both participate in the
    // keymap when the running shell offers a handler.
    auto const catalog = layoutActionCatalog();
    auto const keymap = KeymapModel{uimodel::defaultKeymap()};
    auto const offered = [&catalog](std::string_view const id) { return shippedWindowsAction(catalog, id); };

    auto const plans = planKeymapAccelerators(keymap, catalog, offered);

    auto const planned = [&plans](std::string_view const id)
    { return std::ranges::any_of(plans, [id](auto const& plan) { return plan.actionId == id; }); };

    for (auto const command : {uimodel::PlaybackCommand::PlayPause,
                               uimodel::PlaybackCommand::Next,
                               uimodel::PlaybackCommand::Previous,
                               uimodel::PlaybackCommand::ToggleShuffle,
                               uimodel::PlaybackCommand::CycleRepeat})
    {
      auto const actionId = uimodel::playbackCommandActionId(command);
      INFO(actionId);
      CHECK(planned(actionId));
    }

    // Stop ships bound to the media key alone, which the system media controls
    // already deliver, so the keyboard map installs nothing for it here.
    CHECK_FALSE(planned(uimodel::playbackCommandActionId(uimodel::PlaybackCommand::Stop)));

    CHECK(planned("workspace.revealCurrentTrack"));
    CHECK(planned("track.orderMoveUp"));
    CHECK(planned("track.orderMoveDown"));
    CHECK(planned("track.orderMoveToTop"));
    CHECK(planned("track.orderMoveToBottom"));
  }

  TEST_CASE("KeymapAcceleratorPlan - no two accelerators claim the same key", "[winui][unit][input]")
  {
    auto const catalog = layoutActionCatalog();
    auto const keymap = KeymapModel{uimodel::defaultKeymap()};
    auto const offered = [&catalog](std::string_view const id) { return shippedWindowsAction(catalog, id); };

    auto const plans = planKeymapAccelerators(keymap, catalog, offered);
    auto seen = std::set<std::pair<std::uint32_t, std::uint32_t>>{};

    for (auto const& plan : plans)
    {
      INFO(plan.actionId << " key " << plan.key.virtualKey << " modifiers " << plan.key.modifiers);
      CHECK(seen.emplace(plan.key.virtualKey, plan.key.modifiers).second);
    }
  }
} // namespace ao::winui::test
