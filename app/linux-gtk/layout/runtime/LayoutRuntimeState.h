// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutComponentState.h>

#include <cstdint>
#include <functional>
#include <string>

namespace ao::uimodel
{
  class LayoutComponentStateStore;
}

namespace ao::gtk::layout
{
  /**
   * @brief Mutable layout runtime state shared across a shell's whole lifetime.
   *
   * Holds the active component-state document, its persistence store and the
   * generation guard, the active preset id, and the edit-mode pair
   * (`editMode` + `onNodeMoved`). Owned by ShellLayoutController and borrowed by
   * the per-build LayoutBuildContext. StatefulComponentState retains a pointer to
   * this object so it can write component runtime state back after a build call
   * returns, which is why it must outlive any single build.
   */
  struct LayoutRuntimeState final
  {
    std::string activePresetId{};
    uimodel::LayoutComponentStateDocument componentState{};
    uimodel::LayoutComponentStateStore* componentStateStore = nullptr;

    /**
     * @brief Monotonically incremented when the active component state document is replaced.
     *
     * Components capture the generation at construction time and refuse to write runtime
     * state if the current generation has moved on. This prevents stale components (e.g.
     * destructing during reset/load/save-defaults) from polluting a freshly assigned
     * state document.
     */
    std::uint64_t componentStateGeneration = 1;

    bool editMode = false;
    std::function<void(std::string const& nodeId, std::int32_t xPosition, std::int32_t yPosition)> onNodeMoved{};
  };
} // namespace ao::gtk::layout
