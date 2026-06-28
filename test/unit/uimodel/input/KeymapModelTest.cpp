// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionTypes.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    KeyChord chord(std::string const& text)
    {
      auto const optChord = KeyChord::parse(text);
      REQUIRE(optChord.has_value());
      return *optChord;
    }

    KeymapBindings sampleDefaults()
    {
      return KeymapBindings{
        {"playback.playPause", {chord("Ctrl+P"), chord("Media:Play")}},
        {"playback.next", {chord("Ctrl+Right")}},
      };
    }
  } // namespace

  TEST_CASE("KeymapModel exposes defaults when no overrides applied", "[uimodel][unit][input][keymap]")
  {
    auto const model = KeymapModel{sampleDefaults()};
    CHECK(model.chordsFor("playback.playPause").size() == 2);
    CHECK(model.chordsFor("playback.next") == std::vector<KeyChord>{chord("Ctrl+Right")});
    CHECK(model.chordsFor("unknown").empty());
  }

  TEST_CASE("KeymapModel override replaces a single action", "[uimodel][unit][input][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    auto const diagnostics = model.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+N"}}});

    CHECK(diagnostics.empty());
    CHECK(model.chordsFor("playback.next") == std::vector<KeyChord>{chord("Ctrl+N")});
    // Untouched actions keep defaults.
    CHECK(model.chordsFor("playback.playPause").size() == 2);
  }

  TEST_CASE("KeymapModel empty override list means explicitly unbound", "[uimodel][unit][input][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    model.applyOverrides(KeymapOverrides{{"playback.playPause", {}}});
    CHECK(model.chordsFor("playback.playPause").empty());
  }

  TEST_CASE("KeymapModel reports unparseable override strings", "[uimodel][unit][input][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    auto const diagnostics = model.applyOverrides(KeymapOverrides{{"playback.next", {"Bogus+", "Ctrl+N"}}});

    REQUIRE(diagnostics.size() == 1);
    CHECK(diagnostics.front() == "playback.next: Bogus+");
    CHECK(model.chordsFor("playback.next") == std::vector<KeyChord>{chord("Ctrl+N")});
  }

  TEST_CASE("KeymapModel applyOverrides re-derives from defaults each call", "[uimodel][unit][input][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    model.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+N"}}});
    model.applyOverrides(KeymapOverrides{}); // empty overrides -> back to defaults
    CHECK(model.chordsFor("playback.next") == std::vector<KeyChord>{chord("Ctrl+Right")});
  }

  TEST_CASE("KeymapModel deduplicates chords within an override", "[uimodel][unit][input][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    model.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+N", "ctrl+n"}}});
    CHECK(model.chordsFor("playback.next").size() == 1);
  }

  TEST_CASE("KeymapModel actionFor returns the action bound to a chord", "[uimodel][unit][input][keymap]")
  {
    auto const model = KeymapModel{sampleDefaults()};
    CHECK(model.actionFor(chord("Media:Play")) == "playback.playPause");
    CHECK(model.actionFor(chord("Ctrl+Right")) == "playback.next");
    CHECK(model.actionFor(chord("Ctrl+Z")).has_value() == false);
  }

  TEST_CASE("KeymapModel detects conflicting chord bindings", "[uimodel][unit][input][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    model.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+P"}}}); // collides with playPause

    auto const conflicts = model.conflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts.front().chord == chord("Ctrl+P"));
    CHECK(conflicts.front().actionIds == std::vector<std::string>{"playback.next", "playback.playPause"});
  }

  TEST_CASE("KeymapModel validates action ids against a catalog", "[uimodel][unit][input][keymap]")
  {
    auto catalog = LayoutActionCatalog{};
    catalog.registerActionDescriptor(LayoutActionDescriptor{.id = "playback.playPause", .label = "P", .category = "X"});
    catalog.registerActionDescriptor(LayoutActionDescriptor{.id = "playback.next", .label = "N", .category = "X"});

    auto model = KeymapModel{sampleDefaults()};
    CHECK(model.unknownActionIds(catalog).empty());

    model.applyOverrides(KeymapOverrides{{"playback.bogus", {"Ctrl+B"}}});
    CHECK(model.unknownActionIds(catalog) == std::vector<std::string>{"playback.bogus"});
  }

  TEST_CASE("KeymapModel bind and unbind mutate the effective map", "[uimodel][unit][input][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};

    CHECK(model.bind("playback.next", chord("Ctrl+N")));
    CHECK(model.bindings() == KeymapBindings{{"playback.next", {chord("Ctrl+Right"), chord("Ctrl+N")}},
                                             {"playback.playPause", {chord("Ctrl+P"), chord("Media:Play")}}});
    CHECK(model.bind("playback.next", chord("Ctrl+N")) == false); // duplicate rejected
    CHECK(model.bindings() == KeymapBindings{{"playback.next", {chord("Ctrl+Right"), chord("Ctrl+N")}},
                                             {"playback.playPause", {chord("Ctrl+P"), chord("Media:Play")}}});

    CHECK(model.unbind("playback.next", chord("Ctrl+N")));
    CHECK(model.bindings() == sampleDefaults());
    CHECK(model.unbind("playback.next", chord("Ctrl+N")) == false); // already gone
    CHECK(model.bindings() == sampleDefaults());
  }

  TEST_CASE("KeymapModel reset restores defaults", "[uimodel][unit][input][keymap]")
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

  TEST_CASE("KeymapModel toOverrides returns only deltas", "[uimodel][unit][input][keymap]")
  {
    auto model = KeymapModel{sampleDefaults()};
    CHECK(model.toOverrides().empty()); // identical to defaults

    model.applyOverrides(KeymapOverrides{{"playback.next", {"Ctrl+N"}}});
    auto const overrides = model.toOverrides();
    CHECK(overrides.size() == 1);
    REQUIRE(overrides.count("playback.next") == 1);
    CHECK(overrides.at("playback.next") == std::vector<std::string>{"Ctrl+N"});
  }

  TEST_CASE("defaultKeymap matches the shipped command actions", "[uimodel][unit][input][keymap]")
  {
    auto const defaults = defaultKeymap();
    REQUIRE(defaults.count("playback.playPause") == 1);
    CHECK(defaults.at("playback.playPause").front() == chord("Ctrl+P"));
    CHECK(defaults.count("workspace.revealCurrentTrack") == 1);
  }
} // namespace ao::uimodel::test
