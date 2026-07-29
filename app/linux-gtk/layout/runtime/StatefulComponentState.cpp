// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "StatefulComponentState.h"

#include "LayoutBuildContext.h"
#include "LayoutRuntimeState.h"
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutComponentStateStore.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::layout
{
  StatefulComponentState::StatefulComponentState(LayoutBuildContext& ctx,
                                                 uimodel::LayoutNode const& node,
                                                 std::string_view const type)
    : _state{&ctx.runtimeState}
    , _componentId{node.id}
    , _type{type}
    , _presetId{ctx.buildState.presetId()}
    , _baselineHash{uimodel::componentBaselineHash(node)}
    , _capturedGeneration{ctx.buildState.generation()}
    , _persistable{!ctx.buildState.isEditMode() && ctx.surface == LayoutSurface::Main && !node.id.empty() &&
                   !ctx.buildState.presetId().empty() && ctx.runtimeState.componentStateStore != nullptr}
    , _optRestored{uimodel::resolveComponentState(ctx.buildState.document(), node)}
  {
  }

  bool StatefulComponentState::canWrite() const noexcept
  {
    return _persistable && _state != nullptr && _state->componentStateStore != nullptr &&
           _state->componentStateGeneration == _capturedGeneration;
  }

  void StatefulComponentState::write(std::map<std::string, uimodel::LayoutValue, std::less<>> state)
  {
    if (!canWrite())
    {
      return;
    }

    _state->componentState.preset = _presetId;
    _state->componentState.components[_componentId] = uimodel::LayoutComponentStateEntry{
      .type = _type,
      .stateVersion = uimodel::kStateEntryVersion,
      .baselineHash = _baselineHash,
      .state = std::move(state),
    };
    _state->componentStateStore->save(_presetId, _state->componentState);
  }
} // namespace ao::gtk::layout
