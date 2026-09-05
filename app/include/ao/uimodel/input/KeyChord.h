// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  /**
   * @brief Platform-neutral keyboard modifier flags.
   *
   * These are deliberately decoupled from any toolkit (GDK, WinUI). Frontends
   * translate to/from their native modifier types at the platform boundary.
   */
  enum class KeyModifier : std::uint8_t
  {
    None = 0,
    Ctrl = 1U << 0U,
    Shift = 1U << 1U,
    Alt = 1U << 2U,
    Super = 1U << 3U,
  };

  struct KeyModifiers final
  {
    std::uint8_t mask = 0;

    constexpr KeyModifiers() = default;

    constexpr KeyModifiers(KeyModifier mod)
      : mask{static_cast<std::uint8_t>(mod)}
    {
    }

    constexpr explicit KeyModifiers(std::uint8_t rawMask)
      : mask{rawMask}
    {
    }

    constexpr bool has(KeyModifier mod) const
    {
      auto const val = static_cast<std::uint8_t>(mod);
      return val != 0 && (mask & val) == val;
    }

    constexpr bool isEmpty() const { return mask == 0; }

    constexpr KeyModifiers operator|(KeyModifiers other) const
    {
      return KeyModifiers{static_cast<std::uint8_t>(mask | other.mask)};
    }

    constexpr KeyModifiers& operator|=(KeyModifiers other)
    {
      mask = static_cast<std::uint8_t>(mask | other.mask);
      return *this;
    }

    bool operator==(KeyModifiers const&) const = default;
  };

  constexpr KeyModifiers operator|(KeyModifier lhs, KeyModifier rhs)
  {
    return KeyModifiers{lhs} | KeyModifiers{rhs};
  }

  /// What a media key token starts with, and what makes it one.
  inline constexpr auto kMediaKeyPrefix = std::string_view{"Media:"};

  /**
   * @brief A single keyboard chord: zero or more modifiers plus one key token.
   *
   * The key token is a neutral, canonical string rather than a toolkit keysym:
   *   - single printable ASCII letters are stored uppercase ("P", "U");
   *   - digits and punctuation are stored verbatim ("5", "/");
   *   - named keys use stable spellings ("Right", "Space", "Enter", "PageUp");
   *   - function keys F1 through F24 are stored as F<n>, such as "F12";
   *   - media keys use the "Media:" prefix ("Media:Play", "Media:Next"), which
   *     always carries a name after the colon.
   * Unknown tokens, including frontend-specific names, survive parse/serialize
   * verbatim after trimming.
   *
   * Frontends map the token to a native keysym in their platform translator;
   * this type never references GDK/WinUI symbols.
   */
  struct KeyChord final
  {
    KeyModifiers modifiers{};
    std::string key{};

    bool operator==(KeyChord const&) const = default;

    bool isValid() const noexcept { return !key.empty(); }

    /**
     * @brief Whether this names a transport key rather than an ordinary one.
     *
     * A platform may deliver these to whichever application registered for
     * media control rather than to whichever window has focus, in which case
     * the shell has no accelerator to install for them.
     *
     * The prefix alone names no transport key, so a bare "Media:" is an
     * ordinary token here exactly as it is in canonicalization.
     */
    bool isMediaKey() const noexcept
    {
      return key.size() > kMediaKeyPrefix.size() && std::string_view{key}.starts_with(kMediaKeyPrefix);
    }

    /**
     * @brief Parses a canonical chord string such as "Ctrl+Shift+Right".
     *
     * Modifiers are separated by '+' and precede the key token. Modifier names
     * are case-insensitive and accept common aliases (e.g. "Control"/"Primary"
     * for Ctrl, "Meta"/"Cmd"/"Win" for Super). Named keys, F1-F24, and known
     * Media names are canonicalized case-insensitively; unknown tokens remain
     * verbatim. The literal '+' key is encoded as a trailing "++" (e.g.
     * "Ctrl++"), or a lone "+" with no modifiers. Returns std::nullopt for empty
     * input, a modifier with no following key, or an unrecognized modifier
     * segment.
     */
    static std::optional<KeyChord> parse(std::string_view text);

    /**
     * @brief Renders the canonical chord string (modifiers in Ctrl/Shift/Alt/Super order).
     */
    std::string toString() const;
  };
} // namespace ao::uimodel
