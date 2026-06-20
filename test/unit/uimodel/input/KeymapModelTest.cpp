// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/ActionCatalog.h>
#include <ao/uimodel/layout/ActionTypes.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace ao::uimodel::input::test
{
  namespace
  {
    KeyChord chord(std::string const& text)
    {
      return KeyChord::parse(text).value();
    }

    KeymapBindings sampleDefaults()
    {
      return KeymapBindings{
        {"playback.playPause", {chord("Ctrl+P"), chord("Media:Play")}},
        {"playback.next", {chord("Ctrl+Right")}},
      };
    }
  }

  TEST_CASE("KeymapModel exposes defaults when no overrides applied", "[input][unit][keymap]")
  {
    auto const model = KeymapModel{sampleDefaults()};
    CHECK(model.chordsFor("playback.playPause").size() == 2);
    CHECK(model.chordsFor("playback.next") == std::vector<KeyChord>{chord("Ctrl+Right")});
    CHECK(model.chordsFor("unknown").empty());
  }

  TEST_CASE("KeymapModel override replaces a single action", "[input][unit][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    auto const diagnostics = model.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+N"}}});

    CHECK(diagnostics.empty());
    CHECK(model.chordsFor("playback.next") == std::vector<KeyChord>{chord("Ctrl+N")});
    // Untouched actions keep defaults.
    CHECK(model.chordsFor("playback.playPause").size() == 2);
  }

  TEST_CASE("KeymapModel empty override list means explicitly unbound", "[input][unit][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    model.applyOverrides(KeymapOverrides{{"playback.playPause", {}}});
    CHECK(model.chordsFor("playback.playPause").empty());
  }

  TEST_CASE("KeymapModel reports unparseable override strings", "[input][unit][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    auto const diagnostics = model.applyOverrides(KeymapOverrides{{"playback.next", {"Bogus+", "Ctrl+N"}}});

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front() == "playback.next: Bogus+");
    CHECK(model.chordsFor("playback.next") == std::vector<KeyChord>{chord("Ctrl+N")});
  }

  TEST_CASE("KeymapModel applyOverrides re-derives from defaults each call", "[input][unit][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    model.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+N"}}});
    model.applyOverrides(KeymapOverrides{}); // empty overrides -> back to defaults
    CHECK(model.chordsFor("playback.next") == std::vector<KeyChord>{chord("Ctrl+Right")});
  }

  TEST_CASE("KeymapModel deduplicates chords within an override", "[input][unit][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    model.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+N", "ctrl+n"}}});
    CHECK(model.chordsFor("playback.next").size() == 1);
  }

  TEST_CASE("KeymapModel actionFor reverse lookup", "[input][unit][keymap]")
  {
    auto const model = KeymapModel{sampleDefaults()};
    CHECK(model.actionFor(chord("Media:Play")) == "playback.playPause");
    CHECK(model.actionFor(chord("Ctrl+Right")) == "playback.next");
    CHECK(model.actionFor(chord("Ctrl+Z")).has_value() == false);
  }

  TEST_CASE("KeymapModel detects conflicts", "[input][unit][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    model.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+P"}}}); // collides with playPause

    auto const conflicts = model.conflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts.front().chord == chord("Ctrl+P"));
    CHECK(conflicts.front().actionIds == std::vector<std::string>{"playback.next", "playback.playPause"});
  }

  TEST_CASE("KeymapModel validates action ids against a catalog", "[input][unit][keymap]")
  {
    auto catalog = layout::ActionCatalog{};
    catalog.registerActionDescriptor(
      layout::ActionDescriptor{.id = "playback.playPause", .label = "P", .category = "X"});
    catalog.registerActionDescriptor(layout::ActionDescriptor{.id = "playback.next", .label = "N", .category = "X"});

    auto model = KeymapModel{sampleDefaults()};
    CHECK(model.unknownActionIds(catalog).empty());

    model.applyOverrides(KeymapOverrides{{"playback.bogus", {"Ctrl+B"}}});
    CHECK(model.unknownActionIds(catalog) == std::vector<std::string>{"playback.bogus"});
  }

  TEST_CASE("KeymapModel bind and unbind mutate the effective map", "[input][unit][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};

    CHECK(model.bind("playback.next", chord("Ctrl+N")));
    CHECK(model.chordsFor("playback.next").size() == 2);
    CHECK(model.bind("playback.next", chord("Ctrl+N")) == false); // duplicate rejected

    CHECK(model.unbind("playback.next", chord("Ctrl+N")));
    CHECK(model.chordsFor("playback.next").size() == 1);
    CHECK(model.unbind("playback.next", chord("Ctrl+N")) == false); // already gone
  }

  TEST_CASE("KeymapModel reset restores defaults", "[input][unit][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    model.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+N"}}});

    model.resetToDefault("playback.next");
    CHECK(model.chordsFor("playback.next") == std::vector<KeyChord>{chord("Ctrl+Right")});

    model.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+N"}}, {"playback.playPause", {}}});
    model.resetAllToDefault();
    CHECK(model.chordsFor("playback.playPause").size() == 2);
    CHECK(model.chordsFor("playback.next") == std::vector<KeyChord>{chord("Ctrl+Right")});
  }

  TEST_CASE("KeymapModel toOverrides returns only deltas", "[input][unit][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    CHECK(model.toOverrides().empty()); // identical to defaults

    model.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+N"}}});
    auto const overrides = model.toOverrides();
    REQUIRE(overrides.size() == 1);
    REQUIRE(overrides.count("playback.next") == 1);
    CHECK(overrides.at("playback.next") == std::vector<std::string>{"Ctrl+N"});
  }

  TEST_CASE("defaultKeymap matches the shipped command actions", "[input][unit][keymap]")
  {
    auto const defaults = defaultKeymap();
    REQUIRE(defaults.count("playback.playPause") == 1);
    CHECK(defaults.at("playback.playPause").front() == chord("Ctrl+P"));
    CHECK(defaults.count("workspace.revealCurrentTrack") == 1);
  }
}
