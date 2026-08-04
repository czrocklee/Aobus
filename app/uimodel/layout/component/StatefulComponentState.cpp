// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/component/StatefulComponentState.h>

#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutComponentStateStore.h>
#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/LayoutBuildStateView.h>
#include <ao/uimodel/layout/shell/LayoutRuntimeState.h>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  StatefulComponentState::StatefulComponentState(LayoutRuntimeState& runtimeState,
                                                 LayoutBuildStateView const& buildState,
                                                 LayoutSurface const surface,
                                                 LayoutNode const& node,
                                                 std::string_view const type)
    : _state{&runtimeState}
    , _componentId{node.id}
    , _type{type}
    , _presetId{buildState.presetId()}
    , _baselineHash{componentBaselineHash(node)}
    , _capturedGeneration{buildState.generation()}
    , _persistable{!buildState.isEditMode() && surface == LayoutSurface::Main && !node.id.empty() &&
                   !buildState.presetId().empty() && runtimeState.componentStateStore != nullptr}
    , _optRestored{resolveComponentState(buildState.document(), node)}
  {
  }

  bool StatefulComponentState::canWrite() const noexcept
  {
    return _persistable && _state != nullptr && _state->componentStateStore != nullptr &&
           _state->componentStateGeneration == _capturedGeneration;
  }

  void StatefulComponentState::write(std::map<std::string, LayoutValue, std::less<>> state)
  {
    if (!canWrite())
    {
      return;
    }

    _state->componentState.preset = _presetId;
    _state->componentState.components[_componentId] = LayoutComponentStateEntry{
      .type = _type,
      .stateVersion = kStateEntryVersion,
      .baselineHash = _baselineHash,
      .state = std::move(state),
    };
    _state->componentStateStore->save(_presetId, _state->componentState);
  }
} // namespace ao::uimodel
