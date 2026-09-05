// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/input/KeyChord.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace ao::uimodel::test
{
  namespace
  {
    /// Marks a token the parser rejected, so a canonicalization regression reports a readable
    /// mismatch instead of dereferencing an empty optional.
    constexpr auto kUnparsed = std::string_view{"<unparsed>"};

    std::string parsedKey(std::string_view const text)
    {
      auto const optChord = KeyChord::parse(text);
      return optChord ? optChord->key : std::string{kUnparsed};
    }

    std::string parsedString(std::string_view const text)
    {
      auto const optChord = KeyChord::parse(text);
      return optChord ? optChord->toString() : std::string{kUnparsed};
    }
  } // namespace

  TEST_CASE("KeyChord - parse returns modifiers and key", "[uimodel][unit][input][keychord]")
  {
    SECTION("plain key")
    {
      auto const optChord = KeyChord::parse("P");
      REQUIRE(optChord);
      CHECK(optChord->key == "P");
      CHECK(optChord->modifiers.isEmpty());
    }

    SECTION("single letter normalized to uppercase")
    {
      auto const optChord = KeyChord::parse("Ctrl+p");
      REQUIRE(optChord);
      CHECK(optChord->key == "P");
      CHECK(optChord->modifiers.has(KeyModifier::Ctrl));
    }

    SECTION("multiple modifiers in any input order")
    {
      auto const optChord = KeyChord::parse("shift+ctrl+Right");
      REQUIRE(optChord);
      CHECK(optChord->key == "Right");
      CHECK(optChord->modifiers.has(KeyModifier::Ctrl));
      CHECK(optChord->modifiers.has(KeyModifier::Shift));
      CHECK(optChord->modifiers.has(KeyModifier::Alt) == false);
    }

    SECTION("modifier aliases")
    {
      auto const optPrimary = KeyChord::parse("Primary+L");
      REQUIRE(optPrimary);
      CHECK(optPrimary->modifiers.has(KeyModifier::Ctrl));

      auto const optMetaChord = KeyChord::parse("Meta+Cmd+Q");
      REQUIRE(optMetaChord);
      CHECK(optMetaChord->modifiers.has(KeyModifier::Super));

      auto const optPlatformAliases = KeyChord::parse("Option+Windows+K");
      REQUIRE(optPlatformAliases);
      CHECK(optPlatformAliases->modifiers.has(KeyModifier::Alt));
      CHECK(optPlatformAliases->modifiers.has(KeyModifier::Super));
    }

    SECTION("media key token preserved")
    {
      auto const optChord = KeyChord::parse("Media:Play");
      REQUIRE(optChord);
      CHECK(optChord->key == "Media:Play");
      CHECK(optChord->modifiers.isEmpty());
    }

    SECTION("surrounding whitespace tolerated")
    {
      auto const optChord = KeyChord::parse("  Ctrl + Shift + U  ");
      REQUIRE(optChord);
      CHECK(optChord->key == "U");
      CHECK(optChord->modifiers.has(KeyModifier::Ctrl));
      CHECK(optChord->modifiers.has(KeyModifier::Shift));
    }
  }

  TEST_CASE("KeyChord - parse rejects malformed input", "[uimodel][unit][input][keychord]")
  {
    CHECK(KeyChord::parse("").has_value() == false);
    CHECK(KeyChord::parse("   ").has_value() == false);
    CHECK(KeyChord::parse("Ctrl+").has_value() == false);
    CHECK(KeyChord::parse("Bogus+P").has_value() == false);
  }

  TEST_CASE("KeyChord - toString returns canonical text", "[uimodel][unit][input][keychord]")
  {
    SECTION("modifier order is Ctrl, Shift, Alt, Super")
    {
      auto chord = KeyChord{.modifiers = KeyModifier::Super | KeyModifier::Ctrl | KeyModifier::Shift, .key = "Right"};
      CHECK(chord.toString() == "Ctrl+Shift+Super+Right");
    }

    SECTION("no modifiers")
    {
      CHECK(KeyChord{.key = "Media:Play"}.toString() == "Media:Play");
    }
  }

  TEST_CASE("KeyChord - round-trips through parse and toString", "[uimodel][unit][input][keychord]")
  {
    for (auto const* text :
         {"Ctrl+P", "Ctrl+Shift+Right", "Media:Next", "F5", "Super+Q", "+", "Ctrl++", "Ctrl+Shift++"})
    {
      auto const optChord = KeyChord::parse(text);
      REQUIRE(optChord);
      CHECK(optChord->toString() == text);
    }
  }

  TEST_CASE("KeyChord - parses the '+' key despite the separator collision", "[uimodel][unit][input][keychord]")
  {
    SECTION("bare plus")
    {
      auto const optChord = KeyChord::parse("+");
      REQUIRE(optChord);
      CHECK(optChord->modifiers.isEmpty());
      CHECK(optChord->key == "+");
    }

    SECTION("modified plus")
    {
      auto const optChord = KeyChord::parse("Ctrl++");
      REQUIRE(optChord);
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

  TEST_CASE("KeyChord - equality compares modifiers and key", "[uimodel][unit][input][keychord]")
  {
    CHECK(KeyChord::parse("Ctrl+P") == KeyChord::parse("primary+p"));
    CHECK_FALSE(KeyChord::parse("Ctrl+P") == KeyChord::parse("Ctrl+Shift+P"));
    CHECK_FALSE(KeyChord::parse("Ctrl+P") == KeyChord::parse("Ctrl+Q"));
  }

  TEST_CASE("KeyChord - canonicalizes named keys, function keys, and media aliases", "[uimodel][unit][input][keychord]")
  {
    SECTION("named editing and navigation keys")
    {
      CHECK(parsedKey("return") == "Enter");
      CHECK(parsedKey("ENTER") == "Enter");
      CHECK(parsedKey("esc") == "Escape");
      CHECK(parsedKey("Del") == "Delete");
      CHECK(parsedKey("pgup") == "PageUp");
      CHECK(parsedKey("PgDn") == "PageDown");
      CHECK(parsedKey("pageDown") == "PageDown");
      CHECK(parsedKey("ins") == "Insert");
      CHECK(parsedKey("INSERT") == "Insert");
      CHECK(parsedString("Ctrl+Backspace") == "Ctrl+Backspace");
    }

    SECTION("function keys F1 through F24")
    {
      CHECK(parsedKey("f1") == "F1");
      CHECK(parsedKey("F12") == "F12");
      CHECK(parsedString("f24") == "F24");
      CHECK(parsedKey("F25") == "F25");
      CHECK(parsedKey("f25") == "f25");
    }

    SECTION("known media names and unknown suffixes")
    {
      CHECK(parsedString("media:play") == "Media:Play");
      CHECK(parsedString("MEDIA:PREV") == "Media:Prev");
      CHECK(parsedString("Media:Rewind") == "Media:Rewind");
      CHECK(parsedString("media:rewind") == "Media:rewind");

      // A bare prefix names no transport key. Canonicalization leaves the token alone, and
      // isMediaKey() agrees with it whichever way the prefix was spelled.
      CHECK(parsedKey("Media:") == "Media:");
      CHECK(parsedKey("media:") == "media:");

      for (auto const* const bare : {"Media:", "media:"})
      {
        auto const optBare = KeyChord::parse(bare);
        REQUIRE(optBare);
        CHECK_FALSE(optBare->isMediaKey());
      }

      auto const optPlay = KeyChord::parse("media:play");
      REQUIRE(optPlay);
      CHECK(optPlay->isMediaKey());
    }

    SECTION("unknown tokens survive verbatim")
    {
      CHECK(parsedKey("Hyper") == "Hyper");
      CHECK(parsedKey("GTK_KEY_VoidSymbol") == "GTK_KEY_VoidSymbol");
    }

    SECTION("formerly distinct spellings compare equal after parse")
    {
      CHECK(KeyChord::parse("Return") == KeyChord::parse("enter"));
      CHECK(KeyChord::parse("Esc") == KeyChord::parse("Escape"));
      CHECK(KeyChord::parse("Ctrl+PgUp") == KeyChord::parse("ctrl+pageup"));
    }
  }
} // namespace ao::uimodel::test
