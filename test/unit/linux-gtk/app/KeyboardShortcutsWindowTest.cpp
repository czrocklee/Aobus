// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/KeyboardShortcutsWindow.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/ActionCatalog.h>
#include <ao/uimodel/layout/ActionTypes.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    using uimodel::layout::ActionCapability;

    uimodel::input::KeyChord chord(std::string const& text)
    {
      auto const optChord = uimodel::input::KeyChord::parse(text);
      REQUIRE(optChord.has_value());
      return *optChord;
    }

    uimodel::layout::ActionCatalog makeCatalog()
    {
      auto catalog = uimodel::layout::ActionCatalog{};
      catalog.registerActionDescriptor({.id = "playback.playPause",
                                        .label = "Play/Pause",
                                        .category = "Playback",
                                        .capabilities = ActionCapability::None});
      catalog.registerActionDescriptor(
        {.id = "playback.next", .label = "Next", .category = "Playback", .capabilities = ActionCapability::None});
      // Requires a widget anchor and presents a menu: not drivable by a global accelerator.
      catalog.registerActionDescriptor(
        {.id = "track.editTags",
         .label = "Edit Tags",
         .category = "Tracks",
         .capabilities = ActionCapability::RequiresAnchor | ActionCapability::PresentsMenu});
      return catalog;
    }

    bool contains(std::vector<std::string> const& ids, std::string const& id)
    {
      return std::ranges::contains(ids, id);
    }

    bool hasChord(std::vector<uimodel::input::KeyChord> const& chords, uimodel::input::KeyChord const& c)
    {
      return std::ranges::contains(chords, c);
    }
  }

  TEST_CASE("KeyboardShortcutsWindow lists only shortcut-eligible actions", "[gtk][app][shortcuts]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window =
      KeyboardShortcutsWindow{makeCatalog(), uimodel::input::KeymapModel{uimodel::input::defaultKeymap()}, {}};
    drainGtkEvents();

    auto const& ids = window.editableActionIds();
    CHECK(contains(ids, "playback.playPause"));
    CHECK(contains(ids, "playback.next"));
    CHECK_FALSE(contains(ids, "track.editTags"));
  }

  TEST_CASE("KeyboardShortcutsWindow renders the effective chords for each action", "[gtk][app][shortcuts]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    auto window =
      KeyboardShortcutsWindow{makeCatalog(), uimodel::input::KeymapModel{uimodel::input::defaultKeymap()}, {}};
    drainGtkEvents();

    CHECK(findLabelByText(window, "Ctrl+P") != nullptr);
    CHECK(findLabelByText(window, "Media:Play") != nullptr);
    CHECK(findLabelByText(window, "Play/Pause") != nullptr);
  }

  TEST_CASE("KeyboardShortcutsWindow edits notify and re-render", "[gtk][app][shortcuts]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    std::int32_t changeCount = 0;
    auto optLastModel = std::optional<uimodel::input::KeymapModel>{};
    auto window = KeyboardShortcutsWindow{makeCatalog(),
                                          uimodel::input::KeymapModel{uimodel::input::defaultKeymap()},
                                          [&](uimodel::input::KeymapModel const& model)
                                          {
                                            ++changeCount;
                                            optLastModel = model;
                                          }};
    drainGtkEvents();
    drainGtkEvents();

    SECTION("bindChord adds the chord, notifies, and shows it")
    {
      REQUIRE(window.bindChord("playback.next", chord("Ctrl+N")));
      drainGtkEvents();

      CHECK(changeCount == 1);
      CHECK(window.keymap().actionFor(chord("Ctrl+N")) == std::optional<std::string>{"playback.next"});
      CHECK(findLabelByText(window, "Ctrl+N") != nullptr);
    }

    SECTION("binding an in-use chord transfers it away from the previous owner")
    {
      REQUIRE(window.bindChord("playback.next", chord("Ctrl+P")));
      drainGtkEvents();

      CHECK(hasChord(window.keymap().chordsFor("playback.next"), chord("Ctrl+P")));
      CHECK_FALSE(hasChord(window.keymap().chordsFor("playback.playPause"), chord("Ctrl+P")));
      CHECK(window.keymap().conflicts().empty());
    }

    SECTION("unbindChord removes the chord and its label")
    {
      REQUIRE(window.unbindChord("playback.playPause", chord("Ctrl+P")));
      drainGtkEvents();

      CHECK_FALSE(hasChord(window.keymap().chordsFor("playback.playPause"), chord("Ctrl+P")));
      CHECK(findLabelByText(window, "Ctrl+P") == nullptr);
    }

    SECTION("resetAction restores defaults and reports the updated model")
    {
      REQUIRE(window.bindChord("playback.next", chord("Ctrl+N")));
      window.resetAction("playback.next");

      REQUIRE(optLastModel.has_value());
      CHECK_FALSE(hasChord(optLastModel->chordsFor("playback.next"), chord("Ctrl+N")));
      CHECK(hasChord(window.keymap().chordsFor("playback.next"), chord("Ctrl+Right")));
    }

    SECTION("resetAll reverts every edit")
    {
      window.bindChord("playback.next", chord("Ctrl+N"));
      window.unbindChord("playback.playPause", chord("Ctrl+P"));
      window.resetAll();

      CHECK(hasChord(window.keymap().chordsFor("playback.playPause"), chord("Ctrl+P")));
      CHECK_FALSE(hasChord(window.keymap().chordsFor("playback.next"), chord("Ctrl+N")));
    }
  }

  TEST_CASE("KeyboardShortcutsWindow confirms reassignment of an in-use chord", "[gtk][app][shortcuts]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    std::int32_t changeCount = 0;
    auto window = KeyboardShortcutsWindow{makeCatalog(),
                                          uimodel::input::KeymapModel{uimodel::input::defaultKeymap()},
                                          [&](uimodel::input::KeymapModel const&) { ++changeCount; }};
    drainGtkEvents();

    SECTION("conflictingOwner reports the other action holding the chord")
    {
      CHECK(window.conflictingOwner("playback.next", chord("Ctrl+P")) ==
            std::optional<std::string>{"playback.playPause"});
      CHECK_FALSE(window.conflictingOwner("playback.next", chord("Ctrl+N")).has_value());
      // Re-binding an action's own chord is a no-op, not a conflict.
      CHECK_FALSE(window.conflictingOwner("playback.playPause", chord("Ctrl+P")).has_value());
    }

    SECTION("a free chord binds without prompting")
    {
      bool prompted = false;
      window.setConflictConfirmer([&](std::string const&, std::string const&, std::function<void(bool)>)
                                  { prompted = true; });

      window.requestBind("playback.next", chord("Ctrl+N"));

      CHECK_FALSE(prompted);
      CHECK(hasChord(window.keymap().chordsFor("playback.next"), chord("Ctrl+N")));
    }

    SECTION("an in-use chord prompts and reassigns only on accept")
    {
      auto ownerLabel = std::string{};
      auto chordText = std::string{};
      window.setConflictConfirmer(
        [&](std::string const& owner, std::string const& text, std::function<void(bool)> respond)
        {
          ownerLabel = owner;
          chordText = text;
          respond(true);
        });

      window.requestBind("playback.next", chord("Ctrl+P"));

      CHECK(ownerLabel == "Play/Pause"); // human label, not the raw id
      CHECK(chordText == "Ctrl+P");
      CHECK(hasChord(window.keymap().chordsFor("playback.next"), chord("Ctrl+P")));
      CHECK_FALSE(hasChord(window.keymap().chordsFor("playback.playPause"), chord("Ctrl+P")));
    }

    SECTION("declining the prompt leaves the keymap untouched")
    {
      window.setConflictConfirmer([](std::string const&, std::string const&, std::function<void(bool)> respond)
                                  { respond(false); });

      window.requestBind("playback.next", chord("Ctrl+P"));

      CHECK_FALSE(hasChord(window.keymap().chordsFor("playback.next"), chord("Ctrl+P")));
      CHECK(hasChord(window.keymap().chordsFor("playback.playPause"), chord("Ctrl+P")));
      CHECK(changeCount == 0); // nothing was committed
    }
  }
} // namespace ao::gtk::test
