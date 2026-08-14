// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/uimodel/input/KeyChord.h>

#include <cstdint>
#include <functional>
#include <optional>

namespace ao::winui
{
  /**
   * @brief A chord as Windows names it: one virtual key plus modifier flags.
   *
   * Both fields are the numeric values `VirtualKey` and `VirtualKeyModifiers`
   * carry, which are the Win32 constants those WinRT enumerations were defined
   * from. Keeping them as numbers is what lets the whole translation table be
   * decided and tested without a XAML host: only the last step, handing the
   * pair to a `KeyboardAccelerator`, needs WinRT.
   */
  struct WindowsAccelerator final
  {
    std::uint32_t virtualKey = 0;
    std::uint32_t modifiers = 0;

    bool operator==(WindowsAccelerator const&) const = default;
  };

  /**
   * @name Modifier flags
   *
   * The values of `winrt::Windows::System::VirtualKeyModifiers`.
   * @{
   */
  inline constexpr auto kAcceleratorModifierNone = std::uint32_t{0};
  inline constexpr auto kAcceleratorModifierControl = std::uint32_t{1};
  inline constexpr auto kAcceleratorModifierMenu = std::uint32_t{2};
  inline constexpr auto kAcceleratorModifierShift = std::uint32_t{4};
  inline constexpr auto kAcceleratorModifierWindows = std::uint32_t{8};
  /// @}

  /// A character's key on some keyboard layout, and whether reaching it needs Shift.
  struct CharacterKey final
  {
    std::uint32_t virtualKey = 0;
    bool needsShift = false;

    bool operator==(CharacterKey const&) const = default;
  };

  /**
   * @brief Where a character sits on the keyboard this session is typing on.
   *
   * Windows numbers its punctuation keys by position, and which character a
   * position produces is the active layout's business. A caller that can ask
   * the system supplies this; the fallback below is a US layout, which is right
   * for the common case and wrong for the rest, so the fallback is what a
   * shipped shell should not rely on.
   *
   * Returning nullopt means the layout cannot reach the character with Shift
   * alone - an AltGr composition, say - which is not something a keyboard
   * accelerator can express. Such a chord is skipped rather than installed on
   * whatever key happened to match.
   */
  using CharacterKeyResolver = std::function<std::optional<CharacterKey>(char character)>;

  /**
   * @brief @p chord as a Windows accelerator, or nullopt when Windows has no key for it.
   *
   * Covers the letters, digits, function keys, navigation and editing keys,
   * common punctuation, and the media tokens. A token outside that set is
   * reported as unmappable rather than guessed at, so a shortcut that cannot
   * work on Windows is visibly skipped instead of installed dead.
   *
   * Windows has one media key for play and pause, so `Media:Play` and
   * `Media:Pause` translate to the same key. That the translation exists does
   * not mean a shell should install it: whether a media chord becomes an
   * accelerator at all is `planKeymapAccelerators`' decision, and this shell
   * leaves those keys to the system media controls it already registers for.
   */
  std::optional<WindowsAccelerator> toWindowsAccelerator(uimodel::KeyChord const& chord,
                                                         CharacterKeyResolver const& resolveCharacter = {});
} // namespace ao::winui
