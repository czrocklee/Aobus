// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/input/KeymapModel.h>

namespace ao::rt
{
  class ConfigStore;
}

namespace ao::uimodel::input
{
  /// Config group under which keyboard overrides are persisted.
  inline constexpr char const* kKeymapConfigGroup = "shortcuts";

  /**
   * @brief Loads the effective keymap by merging persisted overrides onto @p defaults.
   *
   * Reads the "shortcuts" group from @p store. A missing group yields the plain
   * defaults. Unparseable chord strings are skipped (logged by the caller via
   * the returned model's diagnostics path is not surfaced here).
   */
  KeymapModel loadKeymap(rt::ConfigStore& store, KeymapBindings defaults);

  /**
   * @brief Persists the keymap's delta-from-defaults into the "shortcuts" group and flushes.
   */
  void saveKeymap(rt::ConfigStore& store, KeymapModel const& keymap);
}
