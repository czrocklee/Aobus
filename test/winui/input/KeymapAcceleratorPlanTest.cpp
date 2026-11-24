// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/input/KeymapAcceleratorPlan.h>

#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/winui/input/KeyChordAccelerator.h>
#include <ao/winui/layout/LayoutSchema.h>

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
    using uimodel::LayoutSchema;

    KeyChord chord(std::string const& text)
    {
      auto optChord = KeyChord::parse(text);
      REQUIRE(optChord);
      return *optChord;
    }

    LayoutSchema schemaWith(std::string id, uimodel::ActionCapabilityMask const capabilities)
    {
      auto schema = LayoutSchema{};
      schema.tryAddAction({.id = std::move(id), .label = "Test", .category = "Test", .capabilities = capabilities});
      return schema;
    }

    /// Answers yes for everything, so a case can isolate the other rules.
    KeymapActionAvailability const kEverythingOffered = [](std::string_view) { return true; };
  } // namespace

  TEST_CASE("KeymapAcceleratorPlan - hints describe only surviving executable bindings")
  {
    auto const keymap = KeymapModel{KeymapBindings{
      {"first", {chord("Ctrl+P")}},
      {"track.orderMoveUp", {KeyChord{.key = "Hyper"}, chord("Ctrl+P"), chord("Ctrl+U"), chord("Alt+Up")}},
      {"track.orderMoveDown", {}},
      {"track.orderMoveToTop", {KeyChord{.key = "Hyper"}}},
      {"track.orderMoveToBottom", {chord("Ctrl+P")}},
    }};
    auto const plans = planKeymapAccelerators(keymap, LayoutSchema{}, kEverythingOffered);
    auto const up = std::ranges::find(plans, std::string{"track.orderMoveUp"}, &KeymapAcceleratorPlan::actionId);
    REQUIRE(up != plans.end());
    CHECK(up->displayText == "Ctrl+U");
    CHECK(up->key.virtualKey == 0x55);

    for (auto const* id : {"track.orderMoveDown", "track.orderMoveToTop", "track.orderMoveToBottom"})
    {
      CHECK(std::ranges::find(plans, std::string{id}, &KeymapAcceleratorPlan::actionId) == plans.end());
    }
  }

  TEST_CASE("KeymapAcceleratorPlan - a binding this shell serves becomes an accelerator")
  {
    auto const keymap = KeymapModel{KeymapBindings{{"playback.playPause", {chord("Ctrl+P")}}}};

    auto const plans = planKeymapAccelerators(keymap, LayoutSchema{}, kEverythingOffered);

    REQUIRE(plans.size() == 1);
    CHECK(plans.front().actionId == "playback.playPause");
    CHECK(plans.front().key.virtualKey == 0x50);
    CHECK(plans.front().key.modifiers == kAcceleratorModifierControl);
  }

  TEST_CASE("KeymapAcceleratorPlan - an action this shell has no handler for is dropped")
  {
    auto const keymap =
      KeymapModel{KeymapBindings{{"playback.playPause", {chord("Ctrl+P")}}, {"track.orderMoveUp", {chord("Alt+Up")}}}};

    auto const plans = planKeymapAccelerators(
      keymap, LayoutSchema{}, [](std::string_view const id) { return id == "playback.playPause"; });

    REQUIRE(plans.size() == 1);
    CHECK(plans.front().actionId == "playback.playPause");
  }

  TEST_CASE("KeymapAcceleratorPlan - shipped anchor requirements exclude custom key bindings")
  {
    auto const schema = layoutSchema();
    REQUIRE(schema.action("library.rescan"));
    REQUIRE_FALSE(schema.action(uimodel::kRevealCurrentTrackActionId));

    // All handlers are offered to isolate the shipped schema's keyboard admission policy.
    auto const keymap = KeymapModel{KeymapBindings{
      {"library.open", {chord("Ctrl+O")}},
      {"shell.showSystemMenu", {chord("Ctrl+M")}},
      {"playback.showOutputDeviceSelector", {chord("Ctrl+D")}},
      {"library.rescan", {chord("Ctrl+R")}},
      {std::string{uimodel::kRevealCurrentTrackActionId}, {chord("Ctrl+L")}},
    }};
    auto const plans = planKeymapAccelerators(keymap, schema, kEverythingOffered);

    CHECK(plans.size() == 2);
    CHECK(std::ranges::contains(plans, std::string{"library.rescan"}, &KeymapAcceleratorPlan::actionId));
    CHECK(std::ranges::contains(
      plans, std::string{uimodel::kRevealCurrentTrackActionId}, &KeymapAcceleratorPlan::actionId));
  }

  TEST_CASE("KeymapAcceleratorPlan - a menu that needs no anchor is kept")
  {
    auto const keymap = KeymapModel{KeymapBindings{{"test.anchorlessMenu", {chord("Ctrl+K")}}}};
    auto const schema =
      schemaWith("test.anchorlessMenu", uimodel::actionCapabilityBit(uimodel::ActionCapability::PresentsMenu));

    CHECK(planKeymapAccelerators(keymap, schema, kEverythingOffered).size() == 1);
  }

  TEST_CASE("KeymapAcceleratorPlan - a chord Windows cannot express is dropped")
  {
    auto const keymap =
      KeymapModel{KeymapBindings{{"playback.playPause", {KeyChord{.key = "Hyper"}, chord("Ctrl+P")}}}};

    auto const plans = planKeymapAccelerators(keymap, LayoutSchema{}, kEverythingOffered);

    REQUIRE(plans.size() == 1);
    CHECK(plans.front().key.virtualKey == 0x50);
  }

  TEST_CASE("KeymapAcceleratorPlan - media chords are left to the system media controls")
  {
    // The shell registers for system media control, which already runs the
    // command. An accelerator on the same key would run it twice while the
    // window has focus.
    auto const keymap =
      KeymapModel{KeymapBindings{{"playback.playPause", {chord("Media:Play"), chord("Media:Pause"), chord("Ctrl+P")}},
                                 {"playback.stop", {chord("Media:Stop")}}}};

    auto const plans = planKeymapAccelerators(keymap, LayoutSchema{}, kEverythingOffered);

    REQUIRE(plans.size() == 1);
    CHECK(plans.front().actionId == "playback.playPause");
    CHECK(plans.front().key.virtualKey == 0x50);
  }

  TEST_CASE("KeymapAcceleratorPlan - two chords on one Windows key yield one accelerator")
  {
    auto const keymap = KeymapModel{KeymapBindings{{"playback.next", {chord("Ctrl+Right"), chord("Ctrl+Right")}}}};

    auto const plans = planKeymapAccelerators(keymap, LayoutSchema{}, kEverythingOffered);

    CHECK(plans.size() == 1);
  }

  TEST_CASE("KeymapAcceleratorPlan - two actions cannot claim one Windows key")
  {
    // `Ctrl++` and `Ctrl+Shift+=` read differently and are the same keystroke,
    // because the character `+` carries its own Shift. Installing both would
    // leave which action runs to XAML's ordering, so the first declaration
    // keeps the key.
    auto const keymap = KeymapModel{KeymapBindings{
      {"playback.playPause", {chord("Ctrl++")}},
      {"playback.stop", {chord("Ctrl+Shift+=")}},
    }};

    auto const plans = planKeymapAccelerators(keymap, LayoutSchema{}, kEverythingOffered);

    REQUIRE(plans.size() == 1);
    CHECK(plans.front().actionId == "playback.playPause");
  }

  TEST_CASE("KeymapAcceleratorPlan - one action keeps every distinct key it asks for")
  {
    // The conflict rule must not swallow a second chord that reaches a
    // different key, which is the ordinary way an action gets two shortcuts.
    auto const keymap =
      KeymapModel{KeymapBindings{{"playback.next", {chord("Ctrl+Right"), chord("Ctrl+N"), chord("Ctrl+Right")}}}};

    auto const plans = planKeymapAccelerators(keymap, LayoutSchema{}, kEverythingOffered);

    CHECK(plans.size() == 2);
  }

  TEST_CASE("KeymapAcceleratorPlan - the default Windows keymap plans caller-offered keyboard commands")
  {
    // Availability is supplied here. Live ShellBuilder registration belongs to
    // the native consumer, not this headless planner test.
    auto const schema = layoutSchema();
    auto const keymap = KeymapModel{uimodel::defaultKeymap()};
    auto const plans = planKeymapAccelerators(keymap, schema, kEverythingOffered);

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

  TEST_CASE("KeymapAcceleratorPlan - no two accelerators claim the same key")
  {
    auto const schema = layoutSchema();
    auto const keymap = KeymapModel{uimodel::defaultKeymap()};
    auto const plans = planKeymapAccelerators(keymap, schema, kEverythingOffered);
    auto seen = std::set<std::pair<std::uint32_t, std::uint32_t>>{};

    for (auto const& plan : plans)
    {
      INFO(plan.actionId << " key " << plan.key.virtualKey << " modifiers " << plan.key.modifiers);
      CHECK(seen.emplace(plan.key.virtualKey, plan.key.modifiers).second);
    }
  }
} // namespace ao::winui::test
