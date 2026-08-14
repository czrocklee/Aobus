// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/input/KeyChordAccelerator.h>

#include <ao/uimodel/input/KeyChord.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace ao::winui
{
  namespace
  {
    using uimodel::KeyChord;
    using uimodel::KeyModifier;

    constexpr auto kVirtualKeyA = std::uint32_t{0x41};
    constexpr auto kVirtualKeyZero = std::uint32_t{0x30};
    constexpr auto kVirtualKeyF1 = std::uint32_t{0x70};
    constexpr auto kHighestFunctionKey = std::uint32_t{24};

    /**
     * @brief Tokens whose Windows key is not derived from the token's own text.
     *
     * Windows names one key for play and pause, so both tokens land on it.
     */
    constexpr auto kNamedKeys = std::to_array<std::pair<std::string_view, std::uint32_t>>({
      {"Backspace", 0x08},  {"Tab", 0x09},        {"Enter", 0x0D},      {"Escape", 0x1B},     {"Space", 0x20},
      {"PageUp", 0x21},     {"PageDown", 0x22},   {"End", 0x23},        {"Home", 0x24},       {"Left", 0x25},
      {"Up", 0x26},         {"Right", 0x27},      {"Down", 0x28},       {"Insert", 0x2D},     {"Delete", 0x2E},
      {"Media:Next", 0xB0}, {"Media:Prev", 0xB1}, {"Media:Stop", 0xB2}, {"Media:Play", 0xB3}, {"Media:Pause", 0xB3},
    });

    /**
     * @brief The punctuation this shell can name, by key position.
     *
     * Windows numbers the OEM keys by where they sit rather than by what they
     * produce, and where they sit is decided by the active keyboard layout.
     * These positions are the ones a US layout carries; on a layout that moves
     * them, a chord naming punctuation reaches a different physical key. The
     * shipped keymap binds no punctuation at all, so this affects only a user's
     * own overrides, and it is recorded here rather than guessed around.
     *
     * `needsShift` marks a character the key only produces while Shift is held.
     * Without it, `Ctrl++` would install as `Ctrl+=`: the same key, a different
     * shortcut, and one the user never asked for.
     */
    struct PunctuationKey final
    {
      char character;
      std::uint32_t virtualKey;
      bool needsShift = false;
    };

    constexpr auto kPunctuationKeys = std::to_array<PunctuationKey>({
      {.character = ';', .virtualKey = 0xBA},  {.character = ':', .virtualKey = 0xBA, .needsShift = true},
      {.character = '=', .virtualKey = 0xBB},  {.character = '+', .virtualKey = 0xBB, .needsShift = true},
      {.character = ',', .virtualKey = 0xBC},  {.character = '<', .virtualKey = 0xBC, .needsShift = true},
      {.character = '-', .virtualKey = 0xBD},  {.character = '_', .virtualKey = 0xBD, .needsShift = true},
      {.character = '.', .virtualKey = 0xBE},  {.character = '>', .virtualKey = 0xBE, .needsShift = true},
      {.character = '/', .virtualKey = 0xBF},  {.character = '?', .virtualKey = 0xBF, .needsShift = true},
      {.character = '`', .virtualKey = 0xC0},  {.character = '~', .virtualKey = 0xC0, .needsShift = true},
      {.character = '[', .virtualKey = 0xDB},  {.character = '{', .virtualKey = 0xDB, .needsShift = true},
      {.character = '\\', .virtualKey = 0xDC}, {.character = '|', .virtualKey = 0xDC, .needsShift = true},
      {.character = ']', .virtualKey = 0xDD},  {.character = '}', .virtualKey = 0xDD, .needsShift = true},
      {.character = '\'', .virtualKey = 0xDE}, {.character = '\"', .virtualKey = 0xDE, .needsShift = true},
    });

    std::optional<std::uint32_t> namedKey(std::string_view const token)
    {
      // std::array's iterator is a raw pointer in libstdc++ but a class type in
      // MSVC's STL, so spelling it as a pointer does not compile on Windows.
      // NOLINTNEXTLINE(readability-qualified-auto)
      auto const it = std::ranges::find(kNamedKeys, token, &std::pair<std::string_view, std::uint32_t>::first);
      return it == kNamedKeys.end() ? std::nullopt : std::optional{it->second};
    }

    std::optional<std::uint32_t> functionKey(std::string_view const token)
    {
      if (token.size() < 2 || token.front() != 'F')
      {
        return std::nullopt;
      }

      std::uint32_t number = 0;
      auto const* const last = token.data() + token.size();

      if (auto const [ptr, ec] = std::from_chars(token.data() + 1, last, number); ec != std::errc{} || ptr != last)
      {
        return std::nullopt;
      }

      if (number == 0 || number > kHighestFunctionKey)
      {
        return std::nullopt;
      }

      return kVirtualKeyF1 + number - 1;
    }

    /// The key a one-character token names, and whether reaching it needs Shift.
    std::optional<PunctuationKey> singleCharacterKey(std::string_view const token)
    {
      if (token.size() != 1)
      {
        return std::nullopt;
      }

      auto const character = token.front();

      // Chord tokens store letters uppercase, so no case folding is needed here.
      if (character >= 'A' && character <= 'Z')
      {
        return PunctuationKey{
          .character = character, .virtualKey = kVirtualKeyA + static_cast<std::uint32_t>(character - 'A')};
      }

      if (character >= '0' && character <= '9')
      {
        return PunctuationKey{
          .character = character, .virtualKey = kVirtualKeyZero + static_cast<std::uint32_t>(character - '0')};
      }

      // As above, the iterator type is a raw pointer only on libstdc++.
      // NOLINTNEXTLINE(readability-qualified-auto)
      auto const it = std::ranges::find(kPunctuationKeys, character, &PunctuationKey::character);
      return it == kPunctuationKeys.end() ? std::nullopt : std::optional{*it};
    }

    /// @p character's key, preferring what the live layout says over the US table.
    std::optional<PunctuationKey> characterKey(std::string_view const token,
                                               CharacterKeyResolver const& resolveCharacter)
    {
      if (token.size() != 1)
      {
        return std::nullopt;
      }

      if (!resolveCharacter)
      {
        return singleCharacterKey(token);
      }

      auto const character = token.front();
      auto const optResolved = resolveCharacter(character);

      // A resolver that answers is authoritative, including when it answers
      // "this layout cannot reach that character". Falling back to the US table
      // there is how a shortcut ends up on the wrong physical key.
      return optResolved ? std::optional{PunctuationKey{.character = character,
                                                        .virtualKey = optResolved->virtualKey,
                                                        .needsShift = optResolved->needsShift}}
                         : std::nullopt;
    }

    std::uint32_t modifierFlags(KeyChord const& chord)
    {
      auto flags = kAcceleratorModifierNone;

      if (chord.modifiers.has(KeyModifier::Ctrl))
      {
        flags |= kAcceleratorModifierControl;
      }

      if (chord.modifiers.has(KeyModifier::Alt))
      {
        flags |= kAcceleratorModifierMenu;
      }

      if (chord.modifiers.has(KeyModifier::Shift))
      {
        flags |= kAcceleratorModifierShift;
      }

      if (chord.modifiers.has(KeyModifier::Super))
      {
        flags |= kAcceleratorModifierWindows;
      }

      return flags;
    }
  } // namespace

  std::optional<WindowsAccelerator> toWindowsAccelerator(KeyChord const& chord,
                                                         CharacterKeyResolver const& resolveCharacter)
  {
    if (!chord.isValid())
    {
      return std::nullopt;
    }

    auto const optKey = [&chord, &resolveCharacter] -> std::optional<PunctuationKey>
    {
      if (auto const optNamed = namedKey(chord.key); optNamed)
      {
        return PunctuationKey{.character = '\0', .virtualKey = *optNamed};
      }

      if (auto const optFunction = functionKey(chord.key); optFunction)
      {
        return PunctuationKey{.character = '\0', .virtualKey = *optFunction};
      }

      return characterKey(chord.key, resolveCharacter);
    }();

    if (!optKey)
    {
      return std::nullopt;
    }

    // A character the key only produces with Shift held carries that Shift
    // whether or not the chord spelled it out. `+` and `=` share one key, and
    // installing `+` without the implicit Shift would silently bind `=`.
    auto modifiers = modifierFlags(chord);

    if (optKey->needsShift)
    {
      modifiers |= kAcceleratorModifierShift;
    }

    return WindowsAccelerator{.virtualKey = optKey->virtualKey, .modifiers = modifiers};
  }
} // namespace ao::winui
