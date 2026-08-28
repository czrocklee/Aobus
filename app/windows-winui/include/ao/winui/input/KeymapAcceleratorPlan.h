// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/winui/input/KeyChordAccelerator.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel
{
  class KeymapModel;
  class LayoutSchema;
} // namespace ao::uimodel

namespace ao::winui
{
  /// One accelerator the shell should install, and the action it runs.
  struct KeymapAcceleratorPlan final
  {
    std::string actionId;
    WindowsAccelerator key;

    bool operator==(KeymapAcceleratorPlan const&) const = default;
  };

  /// Whether this shell has a handler for the named action.
  using KeymapActionAvailability = std::function<bool(std::string_view actionId)>;

  /**
   * @brief The accelerators @p keymap asks for that this shell can actually serve.
   *
   * A keyboard map is written once for the whole application, so most of what
   * it names is not something any one shell can run. Deciding which bindings
   * survive is the whole judgement here, and it is made in plain C++ so it is
   * checked on every host rather than only where XAML exists.
   *
   * A binding is dropped when:
   * - @p isOffered says this shell has no handler for the action;
   * - @p schema says the action presents from an anchor, which a keystroke
   *   does not have, so running it from the keyboard has nowhere to present;
   * - @p resolveCharacter says the active keyboard layout cannot reach the
   *   character with Shift alone, so no accelerator can express it;
   * - the chord names a media key, which Windows delivers to whichever
   *   application registered for system media control rather than to whichever
   *   window has focus. This shell is that application, so an accelerator for
   *   the same key would run the command a second time while it has focus;
   * - Windows has no key for the chord.
   *
   * Every surviving binding becomes one entry, in the keymap's own order. Two
   * chords reaching the same Windows key yield one entry, since a second
   * accelerator for the same key on the same action would never be reached.
   */
  std::vector<KeymapAcceleratorPlan> planKeymapAccelerators(uimodel::KeymapModel const& keymap,
                                                            uimodel::LayoutSchema const& schema,
                                                            KeymapActionAvailability const& isOffered,
                                                            CharacterKeyResolver const& resolveCharacter = {});
} // namespace ao::winui
