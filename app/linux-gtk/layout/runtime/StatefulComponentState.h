// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ao::gtk::layout
{
  struct LayoutBuildContext;
  struct LayoutRuntimeState;

  /**
   * @brief Per-component view over the shared LayoutRuntimeState state document.
   *
   * Reads the restored entry for a node at construction and writes updates back
   * under the runtime state's preset, baseline-hash and generation guards.
   * Retains the LayoutRuntimeState (not the transient LayoutBuildContext) so writes
   * remain valid after the build call returns. Centralises the persistence
   * ritual shared by stateful container components so each one only describes
   * *what* it persists, not *how*.
   */
  class StatefulComponentState final
  {
  public:
    StatefulComponentState(LayoutBuildContext& ctx, uimodel::LayoutNode const& node, std::string_view type);

    /// Runtime state that matched this node's id, type and baseline hash, if any.
    std::optional<uimodel::LayoutComponentStateEntry> const& restored() const noexcept { return _optRestored; }

    /**
     * @brief Whether persisting is still valid for this component.
     *
     * False in edit/tooltip surfaces and for anonymous nodes, and once the
     * context has swapped in a newer state document (reset/load/save-defaults),
     * so a stale component destructing afterwards cannot pollute it.
     */
    bool canWrite() const noexcept;

    /// Stamp @p state into the active document under this component's id and persist it.
    void write(std::map<std::string, uimodel::LayoutValue, std::less<>> state);

  private:
    LayoutRuntimeState* _state = nullptr;
    std::string _componentId;
    std::string _type;
    std::string _presetId;
    std::string _baselineHash;
    std::uint64_t _capturedGeneration = 0;
    bool _persistable = false;
    std::optional<uimodel::LayoutComponentStateEntry> _optRestored;
  };
} // namespace ao::gtk::layout
