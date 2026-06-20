// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/input/KeyChord.h>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::uimodel::layout
{
  class ActionCatalog;
}

namespace ao::uimodel::input
{
  /// Effective bindings: stable action id -> ordered list of chords.
  using KeymapBindings = std::map<std::string, std::vector<KeyChord>, std::less<>>;

  /**
   * @brief Serialized user override form: action id -> chord strings.
   *
   * This is the persisted delta against the defaults. An action present here
   * (even with an empty list) fully replaces the default chords for that action;
   * an empty list therefore means "explicitly unbound". Actions absent here keep
   * their default bindings. The string values are canonical KeyChord spellings.
   */
  using KeymapOverrides = std::map<std::string, std::vector<std::string>, std::less<>>;

  /// A chord bound to more than one action within the effective keymap.
  struct KeymapConflict final
  {
    KeyChord chord;
    std::vector<std::string> actionIds;
  };

  /**
   * @brief Platform-neutral keyboard map: action ids to key chords.
   *
   * The model holds a default keymap plus the effective keymap after user
   * overrides are merged in. All logic here is toolkit-free and unit-testable;
   * frontends translate the resulting chords to native accelerators separately.
   */
  class KeymapModel final
  {
  public:
    KeymapModel() = default;
    explicit KeymapModel(KeymapBindings defaults);

    /**
     * @brief Merges user overrides onto the defaults to form the effective keymap.
     *
     * Each action listed in @p overrides fully replaces that action's chords.
     * Unparseable chord strings are skipped; their canonical "<action>: <text>"
     * descriptions are returned for diagnostics. Calling this re-derives the
     * effective keymap from the defaults each time.
     */
    std::vector<std::string> applyOverrides(KeymapOverrides const& overrides);

    std::vector<KeyChord> chordsFor(std::string_view actionId) const;

    /// Returns the first action bound to @p chord, if any (map order).
    std::optional<std::string> actionFor(KeyChord const& chord) const;

    KeymapBindings const& bindings() const noexcept { return _effective; }
    KeymapBindings const& defaults() const noexcept { return _defaults; }

    /// Chords bound to more than one action, keyed deterministically by chord.
    std::vector<KeymapConflict> conflicts() const;

    /// Effective action ids that are not registered in @p catalog.
    std::vector<std::string> unknownActionIds(layout::ActionCatalog const& catalog) const;

    /// Adds @p chord to @p actionId. Returns false if the chord is invalid or already bound there.
    bool bind(std::string actionId, KeyChord chord);

    /// Removes @p chord from @p actionId. Returns false if it was not bound.
    bool unbind(std::string_view actionId, KeyChord const& chord);

    void resetToDefault(std::string_view actionId);
    void resetAllToDefault();

    /// Produces the persistable delta (effective bindings that differ from defaults).
    KeymapOverrides toOverrides() const;

  private:
    KeymapBindings _defaults;
    KeymapBindings _effective;
  };

  /// Canonical application default keymap (neutral tokens).
  KeymapBindings defaultKeymap();
}
