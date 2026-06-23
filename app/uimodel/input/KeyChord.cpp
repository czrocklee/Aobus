// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/input/KeyChord.h>
#include <ao/utility/String.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::input
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

    std::string canonicalizeKey(std::string_view token)
    {
      auto const trimmed = utility::trim(token);

      // Single ASCII letters are normalized to uppercase for a stable, readable form.
      if (trimmed.size() == 1 && std::isalpha(static_cast<unsigned char>(trimmed.front())) != 0)
      {
        return std::string{static_cast<char>(std::toupper(static_cast<unsigned char>(trimmed.front())))};
      }

      return std::string{trimmed};
    }
  }

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
      auto const pos = trimmed.find('+', start);

      if (pos == std::string_view::npos)
      {
        segments.push_back(trimmed.substr(start));
        break;
      }

      segments.push_back(trimmed.substr(start, pos - start));
      start = pos + 1;
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
}
