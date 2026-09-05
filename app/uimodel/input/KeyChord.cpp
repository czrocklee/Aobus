// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/input/KeyChord.h>

#include <ao/utility/String.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    std::optional<KeyModifier> parseModifier(std::string_view segment)
    {
      auto const lower = utility::toLower(segment);

      if (lower == "ctrl" || lower == "control" || lower == "primary")
      {
        return KeyModifier::Ctrl;
      }

      if (lower == "shift")
      {
        return KeyModifier::Shift;
      }

      if (lower == "alt" || lower == "option")
      {
        return KeyModifier::Alt;
      }

      if (lower == "super" || lower == "meta" || lower == "cmd" || lower == "win" || lower == "windows")
      {
        return KeyModifier::Super;
      }

      return std::nullopt;
    }

    std::string canonicalizeFunctionKey(std::string_view const lower)
    {
      if (lower.size() < 2 || lower.front() != 'f')
      {
        return {};
      }

      auto const digits = lower.substr(1);
      std::uint32_t value = 0;
      auto const* const begin = digits.data();
      auto const* const end = begin + digits.size();
      auto const [ptr, ec] = std::from_chars(begin, end, value);

      if (ec != std::errc{} || ptr != end || value < 1U || value > 24U)
      {
        return {};
      }

      return "F" + std::to_string(value);
    }

    std::string canonicalizeMediaKey(std::string_view const trimmed, std::string_view const lower)
    {
      constexpr auto kPrefixSize = kMediaKeyPrefix.size();

      // A bare "Media:" names no transport key, so it stays an ordinary verbatim token.
      if (!lower.starts_with("media:") || lower.size() <= kPrefixSize)
      {
        return {};
      }

      auto const suffix = trimmed.substr(kPrefixSize);
      auto const suffixLower = lower.substr(kPrefixSize);
      auto canonicalSuffix = std::string{suffix};

      if (suffixLower == "play")
      {
        canonicalSuffix = "Play";
      }
      else if (suffixLower == "pause")
      {
        canonicalSuffix = "Pause";
      }
      else if (suffixLower == "stop")
      {
        canonicalSuffix = "Stop";
      }
      else if (suffixLower == "next")
      {
        canonicalSuffix = "Next";
      }
      else if (suffixLower == "prev")
      {
        canonicalSuffix = "Prev";
      }

      return std::string{kMediaKeyPrefix} + canonicalSuffix;
    }

    std::string canonicalizeKey(std::string_view const token)
    {
      auto const trimmed = utility::trim(token);

      // Single ASCII letters are normalized to uppercase for a stable, readable form.
      if (trimmed.size() == 1 && utility::isAsciiAlpha(trimmed.front()))
      {
        return std::string{utility::toAsciiUpper(trimmed.front())};
      }

      auto const lower = utility::toLower(trimmed);

      if (auto media = canonicalizeMediaKey(trimmed, lower); !media.empty())
      {
        return media;
      }

      if (auto functionKey = canonicalizeFunctionKey(lower); !functionKey.empty())
      {
        return functionKey;
      }

      using NamedKey = std::pair<std::string_view, std::string_view>;
      static constexpr auto kNamedKeys = std::to_array<NamedKey>({
        {"enter", "Enter"},     {"return", "Enter"},  {"escape", "Escape"},       {"esc", "Escape"},
        {"delete", "Delete"},   {"del", "Delete"},    {"backspace", "Backspace"}, {"tab", "Tab"},
        {"space", "Space"},     {"insert", "Insert"}, {"ins", "Insert"},          {"left", "Left"},
        {"right", "Right"},     {"up", "Up"},         {"down", "Down"},           {"home", "Home"},
        {"end", "End"},         {"pageup", "PageUp"}, {"pgup", "PageUp"},         {"pagedown", "PageDown"},
        {"pgdown", "PageDown"}, {"pgdn", "PageDown"},
      });

      for (auto const& [alias, canonical] : kNamedKeys)
      {
        if (lower == alias)
        {
          return std::string{canonical};
        }
      }

      return std::string{trimmed};
    }
  } // namespace

  std::optional<KeyChord> KeyChord::parse(std::string_view text)
  {
    auto const trimmed = ao::utility::trim(text);

    if (trimmed.empty())
    {
      return std::nullopt;
    }

    // Split on '+'. All segments but the last must be modifiers; the last is the key token.
    auto segments = std::vector<std::string_view>{};
    std::size_t start = std::size_t{0};

    while (true)
    {
      auto const separatorOffset = trimmed.find('+', start);

      if (separatorOffset == std::string_view::npos)
      {
        segments.push_back(trimmed.substr(start));
        break;
      }

      segments.push_back(trimmed.substr(start, separatorOffset - start));
      start = separatorOffset + 1;
    }

    // The literal '+' key collides with the separator: toString() writes it as a trailing "++"
    // (or a lone "+"), producing two empty trailing segments. Recover it explicitly; every other
    // key token is simply the final segment, with the leading segments being modifiers.
    auto keyToken = std::string_view{};
    std::size_t modifierCount = std::size_t{0};

    if (segments.size() >= 2 && segments.back().empty() && segments[segments.size() - 2].empty())
    {
      keyToken = "+";
      modifierCount = segments.size() - 2;
    }
    else
    {
      keyToken = segments.back();
      modifierCount = segments.size() - 1;
    }

    auto chord = KeyChord{};

    for (std::size_t index = std::size_t{0}; index < modifierCount; ++index)
    {
      auto const optModifier = parseModifier(ao::utility::trim(segments[index]));

      if (!optModifier)
      {
        return std::nullopt;
      }

      chord.modifiers |= *optModifier;
    }

    chord.key = canonicalizeKey(keyToken);

    if (chord.key.empty())
    {
      return std::nullopt;
    }

    return chord;
  }

  std::string KeyChord::toString() const
  {
    static constexpr auto kOrdered =
      std::array<std::pair<KeyModifier, std::string_view>, 4>{{{KeyModifier::Ctrl, "Ctrl"},
                                                               {KeyModifier::Shift, "Shift"},
                                                               {KeyModifier::Alt, "Alt"},
                                                               {KeyModifier::Super, "Super"}}};

    auto result = std::string{};

    for (auto const& [modifier, name] : kOrdered)
    {
      if (modifiers.has(modifier))
      {
        result.append(name);
        result.push_back('+');
      }
    }

    result.append(key);
    return result;
  }
} // namespace ao::uimodel
