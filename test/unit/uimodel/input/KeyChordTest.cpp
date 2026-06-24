// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/input/KeyChord.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::uimodel::input::test
{
  TEST_CASE("KeyChord parse modifiers and key", "[input][unit][keychord]")
  {
    SECTION("plain key")
    {
      auto const optChord = KeyChord::parse("P");
      REQUIRE(optChord.has_value());
      CHECK(optChord->key == "P");
      CHECK(optChord->modifiers.empty());
    }

    SECTION("single letter normalized to uppercase")
    {
      auto const optChord = KeyChord::parse("Ctrl+p");
      REQUIRE(optChord.has_value());
      CHECK(optChord->key == "P");
      CHECK(optChord->modifiers.has(KeyModifier::Ctrl));
    }

    SECTION("multiple modifiers in any input order")
    {
      auto const optChord = KeyChord::parse("shift+ctrl+Right");
      REQUIRE(optChord.has_value());
      CHECK(optChord->key == "Right");
      CHECK(optChord->modifiers.has(KeyModifier::Ctrl));
      CHECK(optChord->modifiers.has(KeyModifier::Shift));
      CHECK(optChord->modifiers.has(KeyModifier::Alt) == false);
    }

    SECTION("modifier aliases")
    {
      auto const optPrimary = KeyChord::parse("Primary+L");
      REQUIRE(optPrimary.has_value());
      CHECK(optPrimary->modifiers.has(KeyModifier::Ctrl));

      auto const optMeta = KeyChord::parse("Meta+Cmd+Q");
      REQUIRE(optMeta.has_value());
      CHECK(optMeta->modifiers.has(KeyModifier::Super));
    }

    SECTION("media key token preserved")
    {
      auto const optChord = KeyChord::parse("Media:Play");
      REQUIRE(optChord.has_value());
      CHECK(optChord->key == "Media:Play");
      CHECK(optChord->modifiers.empty());
    }

    SECTION("surrounding whitespace tolerated")
    {
      auto const optChord = KeyChord::parse("  Ctrl + Shift + U  ");
      REQUIRE(optChord.has_value());
      CHECK(optChord->key == "U");
      CHECK(optChord->modifiers.has(KeyModifier::Ctrl));
      CHECK(optChord->modifiers.has(KeyModifier::Shift));
    }
  }

  TEST_CASE("KeyChord parse rejects malformed input", "[input][unit][keychord]")
  {
    CHECK(KeyChord::parse("").has_value() == false);
    CHECK(KeyChord::parse("   ").has_value() == false);
    CHECK(KeyChord::parse("Ctrl+").has_value() == false);
    CHECK(KeyChord::parse("Bogus+P").has_value() == false);
  }

  TEST_CASE("KeyChord toString is canonical", "[input][unit][keychord]")
  {
    SECTION("modifier order is Ctrl, Shift, Alt, Super")
    {
      auto chord = KeyChord{.modifiers = KeyModifier::Super | KeyModifier::Ctrl | KeyModifier::Shift, .key = "Right"};
      CHECK(chord.toString() == "Ctrl+Shift+Super+Right");
    }

    SECTION("no modifiers")
    {
      CHECK(KeyChord{.modifiers = {}, .key = "Media:Play"}.toString() == "Media:Play");
    }
  }

  TEST_CASE("KeyChord round-trips through parse/toString", "[input][unit][keychord]")
  {
    for (auto const* text :
         {"Ctrl+P", "Ctrl+Shift+Right", "Media:Next", "F5", "Super+Q", "+", "Ctrl++", "Ctrl+Shift++"})
    {
      auto const optChord = KeyChord::parse(text);
      REQUIRE(optChord.has_value());
      CHECK(optChord->toString() == text);
    }
  }

  TEST_CASE("KeyChord parses the '+' key despite the separator collision", "[input][unit][keychord]")
  {
    SECTION("bare plus")
    {
      auto const optChord = KeyChord::parse("+");
      REQUIRE(optChord.has_value());
      CHECK(optChord->modifiers.empty());
      CHECK(optChord->key == "+");
    }

    SECTION("modified plus")
    {
      auto const optChord = KeyChord::parse("Ctrl++");
      REQUIRE(optChord.has_value());
      CHECK(optChord->modifiers.has(KeyModifier::Ctrl));
      CHECK(optChord->key == "+");
    }

    SECTION("a dangling modifier with no key is still rejected")
    {
      // "Ctrl+" must stay invalid (modifier with nothing after it), distinct from "Ctrl++".
      CHECK(KeyChord::parse("Ctrl+").has_value() == false);
      CHECK(KeyChord::parse("Ctrl+Shift+").has_value() == false);
    }
  }

  TEST_CASE("KeyChord equality compares modifiers and key", "[input][unit][keychord]")
  {
    CHECK(KeyChord::parse("Ctrl+P") == KeyChord::parse("primary+p"));
    CHECK_FALSE(KeyChord::parse("Ctrl+P") == KeyChord::parse("Ctrl+Shift+P"));
    CHECK_FALSE(KeyChord::parse("Ctrl+P") == KeyChord::parse("Ctrl+Q"));
  }
} // namespace ao::uimodel::input::test
