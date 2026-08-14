// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/input/KeyChordAccelerator.h>

#include <ao/uimodel/input/KeyChord.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::winui::test
{
  namespace
  {
    using uimodel::KeyChord;

    WindowsAccelerator translated(std::string const& text)
    {
      auto const optChord = KeyChord::parse(text);
      REQUIRE(optChord);
      auto const optAccelerator = toWindowsAccelerator(*optChord);
      REQUIRE(optAccelerator);
      return *optAccelerator;
    }
  } // namespace

  TEST_CASE("KeyChordAccelerator - letters and digits take their own virtual key", "[winui][unit][input]")
  {
    CHECK(translated("P").virtualKey == 0x50);
    CHECK(translated("A").virtualKey == 0x41);
    CHECK(translated("Z").virtualKey == 0x5A);
    CHECK(translated("0").virtualKey == 0x30);
    CHECK(translated("9").virtualKey == 0x39);
  }

  TEST_CASE("KeyChordAccelerator - modifiers become the Windows flags", "[winui][unit][input]")
  {
    CHECK(translated("Ctrl+P").modifiers == kAcceleratorModifierControl);
    CHECK(translated("Alt+Up").modifiers == kAcceleratorModifierMenu);
    CHECK(translated("Shift+Home").modifiers == kAcceleratorModifierShift);
    CHECK(translated("Super+L").modifiers == kAcceleratorModifierWindows);
    CHECK(translated("Ctrl+Shift+Right").modifiers == (kAcceleratorModifierControl | kAcceleratorModifierShift));
    CHECK(translated("Right").modifiers == kAcceleratorModifierNone);
  }

  TEST_CASE("KeyChordAccelerator - named navigation and editing keys translate", "[winui][unit][input]")
  {
    CHECK(translated("Left").virtualKey == 0x25);
    CHECK(translated("Up").virtualKey == 0x26);
    CHECK(translated("Right").virtualKey == 0x27);
    CHECK(translated("Down").virtualKey == 0x28);
    CHECK(translated("Home").virtualKey == 0x24);
    CHECK(translated("End").virtualKey == 0x23);
    CHECK(translated("PageUp").virtualKey == 0x21);
    CHECK(translated("PageDown").virtualKey == 0x22);
    CHECK(translated("Space").virtualKey == 0x20);
    CHECK(translated("Enter").virtualKey == 0x0D);
    CHECK(translated("Backspace").virtualKey == 0x08);
    CHECK(translated("Delete").virtualKey == 0x2E);
  }

  TEST_CASE("KeyChordAccelerator - function keys are numbered from F1", "[winui][unit][input]")
  {
    CHECK(translated("F1").virtualKey == 0x70);
    CHECK(translated("F5").virtualKey == 0x74);
    CHECK(translated("F24").virtualKey == 0x87);

    // Past the last function key Windows names, and a token that only looks like one.
    CHECK_FALSE(toWindowsAccelerator(KeyChord{.key = "F25"}));
    CHECK_FALSE(toWindowsAccelerator(KeyChord{.key = "F0"}));
    CHECK_FALSE(toWindowsAccelerator(KeyChord{.key = "F5x"}));
  }

  TEST_CASE("KeyChordAccelerator - Windows names one key for play and pause", "[winui][unit][input]")
  {
    // GDK separates them; Windows does not, so both tokens have to land
    // somewhere real rather than one of them being dropped.
    CHECK(translated("Media:Play").virtualKey == 0xB3);
    CHECK(translated("Media:Pause").virtualKey == 0xB3);
    CHECK(translated("Media:Next").virtualKey == 0xB0);
    CHECK(translated("Media:Prev").virtualKey == 0xB1);
    CHECK(translated("Media:Stop").virtualKey == 0xB2);
  }

  TEST_CASE("KeyChordAccelerator - a token Windows has no key for is unmappable", "[winui][unit][input]")
  {
    // Reported rather than guessed at: an accelerator built from a guess is one
    // that silently never fires.
    CHECK_FALSE(toWindowsAccelerator(KeyChord{}));
    CHECK_FALSE(toWindowsAccelerator(KeyChord{.key = "Hyper"}));
    CHECK_FALSE(toWindowsAccelerator(KeyChord{.key = "Media:Rewind"}));
    CHECK_FALSE(toWindowsAccelerator(KeyChord{.key = "AudioPlay"}));
  }

  TEST_CASE("KeyChordAccelerator - every default binding reaches a Windows key", "[winui][unit][input]")
  {
    // The shipped defaults are the shortcuts a Windows user gets without ever
    // opening a settings page, so none of them may be silently unmappable.
    for (auto const& [actionId, chords] : uimodel::defaultKeymap())
    {
      for (auto const& chord : chords)
      {
        INFO(actionId << " -> " << chord.toString());
        CHECK(toWindowsAccelerator(chord).has_value());
      }
    }
  }

  TEST_CASE("KeyChordAccelerator - a shifted character carries the Shift it needs", "[winui][unit][input]")
  {
    // Windows numbers the OEM keys by position, so `+` and `=` are one key.
    // Translating `+` without its implicit Shift installs `=`: the same
    // keystroke the user did not ask for, and a shortcut that looks bound in
    // the keymap while doing something else.
    auto const plus = translated("Ctrl++");
    CHECK(plus.virtualKey == 0xBB);
    CHECK(plus.modifiers == (kAcceleratorModifierControl | kAcceleratorModifierShift));

    auto const equals = translated("Ctrl+=");
    CHECK(equals.virtualKey == 0xBB);
    CHECK(equals.modifiers == kAcceleratorModifierControl);

    // The two are the same key and must stay distinguishable.
    CHECK_FALSE(plus == equals);
  }

  TEST_CASE("KeyChordAccelerator - punctuation reaches its US key position", "[winui][unit][input]")
  {
    struct Expectation final
    {
      std::string_view chordText;
      std::uint32_t virtualKey;
      std::uint32_t modifiers;
    };

    auto const cases = std::to_array<Expectation>({
      {.chordText = "Ctrl+;", .virtualKey = 0xBA, .modifiers = kAcceleratorModifierControl},
      {.chordText = "Ctrl+,", .virtualKey = 0xBC, .modifiers = kAcceleratorModifierControl},
      {.chordText = "Ctrl+-", .virtualKey = 0xBD, .modifiers = kAcceleratorModifierControl},
      {.chordText = "Ctrl+.", .virtualKey = 0xBE, .modifiers = kAcceleratorModifierControl},
      {.chordText = "Ctrl+/", .virtualKey = 0xBF, .modifiers = kAcceleratorModifierControl},
      {.chordText = "Ctrl+[", .virtualKey = 0xDB, .modifiers = kAcceleratorModifierControl},
      {.chordText = "Ctrl+]", .virtualKey = 0xDD, .modifiers = kAcceleratorModifierControl},
      {.chordText = "Ctrl+?", .virtualKey = 0xBF, .modifiers = kAcceleratorModifierControl | kAcceleratorModifierShift},
      {.chordText = "Ctrl+_", .virtualKey = 0xBD, .modifiers = kAcceleratorModifierControl | kAcceleratorModifierShift},
    });

    for (auto const& expected : cases)
    {
      INFO(expected.chordText);
      auto const key = translated(std::string{expected.chordText});
      CHECK(key.virtualKey == expected.virtualKey);
      CHECK(key.modifiers == expected.modifiers);
    }
  }

  TEST_CASE("KeyChordAccelerator - spelling a Shift that the character already implies changes nothing",
            "[winui][unit][input]")
  {
    // `Shift+=` and `+` are one keystroke described two ways, so they have to
    // translate identically or the conflict check cannot see them collide.
    CHECK(translated("Ctrl+Shift+=") == translated("Ctrl++"));
  }

  TEST_CASE("KeyChordAccelerator - a layout resolver decides where a character sits", "[winui][unit][input]")
  {
    // The built-in table is a US keyboard. A shell installing real accelerators
    // asks the system instead, because Windows names punctuation keys by
    // position and only the system knows which position carries which
    // character.
    auto const german = [](char const character) -> std::optional<CharacterKey>
    {
      // On a German layout `+` is its own key, unshifted, where a US keyboard
      // has `]`.
      if (character == '+')
      {
        return CharacterKey{.virtualKey = 0xDD, .needsShift = false};
      }

      return std::nullopt;
    };

    auto const optChord = uimodel::KeyChord::parse("Ctrl++");
    REQUIRE(optChord);

    auto const optResolved = toWindowsAccelerator(*optChord, german);
    REQUIRE(optResolved);
    CHECK(optResolved->virtualKey == 0xDD);
    CHECK(optResolved->modifiers == kAcceleratorModifierControl);

    // Without the resolver the same chord lands on the US position instead,
    // which is exactly the divergence the resolver exists to remove.
    CHECK(translated("Ctrl++").virtualKey == 0xBB);
  }

  TEST_CASE("KeyChordAccelerator - a character the layout cannot reach installs nothing", "[winui][unit][input]")
  {
    // A resolver that answers is authoritative, including when it says the
    // character needs AltGr. Falling back to the US table there would bind a
    // key the user never pressed.
    auto const unreachable = [](char) -> std::optional<CharacterKey> { return std::nullopt; };

    auto const optChord = uimodel::KeyChord::parse("Ctrl+@");
    REQUIRE(optChord);
    CHECK_FALSE(toWindowsAccelerator(*optChord, unreachable));

    // Named keys are layout-independent and stay unaffected.
    auto const optNamed = uimodel::KeyChord::parse("Ctrl+Left");
    REQUIRE(optNamed);
    CHECK(toWindowsAccelerator(*optNamed, unreachable));
  }
} // namespace ao::winui::test