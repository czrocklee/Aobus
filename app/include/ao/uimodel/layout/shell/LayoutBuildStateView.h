// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/shell/LayoutRuntimeState.h>

#include <cstdint>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  /// State read while constructing one tree: either the live carrier or an explicit replacement candidate.
  class LayoutBuildStateView final
  {
  public:
    explicit LayoutBuildStateView(LayoutRuntimeState const& state)
      : _runtimeState{&state}
    {
    }

    LayoutBuildStateView(std::string_view preset,
                         LayoutComponentStateDocument const& stateDocument,
                         std::uint64_t stateGeneration,
                         bool isEditMode = false,
                         LayoutNodeMovedFn nodeMoved = {})
      : _presetId{preset}
      , _document{&stateDocument}
      , _generation{stateGeneration}
      , _editMode{isEditMode}
      , _onNodeMoved{std::move(nodeMoved)}
      , _hasGenerationOverride{true}
    {
    }

    std::string_view presetId() const noexcept
    {
      return _runtimeState == nullptr ? _presetId : std::string_view{_runtimeState->activePresetId};
    }

    LayoutComponentStateDocument const& document() const noexcept
    {
      return _runtimeState == nullptr ? *_document : _runtimeState->componentState;
    }

    std::uint64_t generation() const noexcept;

    bool isEditMode() const noexcept { return _runtimeState == nullptr ? _editMode : _runtimeState->editMode; }

    LayoutNodeMovedFn const& onNodeMoved() const noexcept
    {
      return _runtimeState == nullptr ? _onNodeMoved : _runtimeState->onNodeMoved;
    }

    void overrideGeneration(std::uint64_t generation) noexcept;

  private:
    LayoutRuntimeState const* _runtimeState = nullptr;
    std::string_view _presetId{};
    LayoutComponentStateDocument const* _document = nullptr;
    std::uint64_t _generation = 0;
    bool _editMode = false;
    LayoutNodeMovedFn _onNodeMoved{};
    bool _hasGenerationOverride = false;
  };
} // namespace ao::uimodel
