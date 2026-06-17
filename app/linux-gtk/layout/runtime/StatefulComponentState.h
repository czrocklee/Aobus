// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutContext.h"
#include <ao/uimodel/layout/ILayoutComponentStateStore.h>
#include <ao/uimodel/layout/LayoutComponentState.h>
#include <ao/uimodel/layout/LayoutNode.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::layout
{
  /**
   * @brief Per-component view over the active LayoutContext runtime-state document.
   *
   * Reads the restored entry for a node at construction and writes updates back
   * under the context's preset, baseline-hash and generation guards. Centralises
   * the persistence ritual shared by stateful container components so each one
   * only describes *what* it persists, not *how*.
   */
  class StatefulComponentState final
  {
  public:
    StatefulComponentState(LayoutContext& ctx, uimodel::layout::LayoutNode const& node, std::string_view type)
      : _ctx{&ctx}
      , _componentId{node.id}
      , _type{type}
      , _presetId{ctx.activePresetId}
      , _baselineHash{uimodel::layout::layoutComponentBaselineHash(node)}
      , _capturedGeneration{ctx.componentStateGeneration}
      , _persistable{!ctx.editMode && ctx.surface == LayoutSurface::Main && !node.id.empty() &&
                     !ctx.activePresetId.empty() && ctx.componentStateStore != nullptr}
      , _optRestored{uimodel::layout::resolveLayoutComponentState(ctx.componentState, node)}
    {
    }

    /// Runtime state that matched this node's id, type and baseline hash, if any.
    std::optional<uimodel::layout::LayoutComponentStateEntry> const& restored() const noexcept { return _optRestored; }

    /**
     * @brief Whether persisting is still valid for this component.
     *
     * False in edit/tooltip surfaces and for anonymous nodes, and once the
     * context has swapped in a newer state document (reset/load/save-defaults),
     * so a stale component destructing afterwards cannot pollute it.
     */
    bool canWrite() const noexcept
    {
      return _persistable && _ctx != nullptr && _ctx->componentStateStore != nullptr &&
             _ctx->componentStateGeneration == _capturedGeneration;
    }

    /// Stamp @p state into the active document under this component's id and persist it.
    void write(std::map<std::string, uimodel::layout::LayoutValue, std::less<>> state)
    {
      if (!canWrite())
      {
        return;
      }

      _ctx->componentState.preset = _presetId;
      _ctx->componentState.components[_componentId] = uimodel::layout::LayoutComponentStateEntry{
        .type = _type,
        .stateVersion = uimodel::layout::kLayoutComponentStateEntryVersion,
        .baselineHash = _baselineHash,
        .state = std::move(state),
      };
      _ctx->componentStateStore->save(_presetId, _ctx->componentState);
    }

  private:
    LayoutContext* _ctx = nullptr;
    std::string _componentId;
    std::string _type;
    std::string _presetId;
    std::string _baselineHash;
    std::uint64_t _capturedGeneration = 0;
    bool _persistable = false;
    std::optional<uimodel::layout::LayoutComponentStateEntry> _optRestored;
  };
} // namespace ao::gtk::layout
