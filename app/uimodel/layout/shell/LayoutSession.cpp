// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/layout/shell/LayoutSession.h>

#include <ao/Contract.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutComponentStateStore.h>
#include <ao/uimodel/layout/component/LayoutStatePromoter.h>
#include <ao/uimodel/layout/component/LayoutSurface.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  LayoutBuildSnapshot::LayoutBuildSnapshot(LayoutComponentStateDocument componentState,
                                           std::uint64_t const generation,
                                           bool const editMode,
                                           LayoutNodeMovedFn onNodeMoved)
    : _componentState{std::move(componentState)}
    , _generation{generation}
    , _editMode{editMode}
    , _onNodeMoved{std::move(onNodeMoved)}
  {
  }

  ComponentStateBinding::ComponentStateBinding(LayoutSession& session,
                                               LayoutBuildSnapshot const& snapshot,
                                               LayoutSurface const surface,
                                               LayoutNode const& node,
                                               std::string_view const type)
    : _session{&session}
    , _componentId{node.id}
    , _type{type}
    , _presetId{snapshot.presetId()}
    , _baselineHash{componentBaselineHash(node)}
    , _generation{snapshot.generation()}
    , _persistable{!snapshot.isEditMode() && surface == LayoutSurface::Main && !node.id.empty() &&
                   !snapshot.presetId().empty() && session._componentStateStore != nullptr}
    , _optRestored{resolveComponentState(snapshot.componentState(), node)}
  {
  }

  bool ComponentStateBinding::canWrite() const noexcept
  {
    return _persistable && _session != nullptr && _session->_componentStateStore != nullptr &&
           _session->_generation == _generation && _session->presetId() == _presetId;
  }

  void ComponentStateBinding::write(std::map<std::string, LayoutValue, std::less<>> state)
  {
    if (!canWrite())
    {
      return;
    }

    _session->_componentState.components[_componentId] = LayoutComponentStateEntry{
      .type = _type,
      .stateVersion = kStateEntryVersion,
      .baselineHash = _baselineHash,
      .state = std::move(state),
    };
    _session->_componentStateStore->save(_presetId, _session->_componentState);
  }

  LayoutSession::LayoutSession(LayoutComponentStateStore* const componentStateStore)
    : _componentStateStore{componentStateStore}
  {
  }

  LayoutPresetSelection LayoutSession::selectPreset(std::string_view const requestedPresetId,
                                                    std::span<std::string_view const> const supportedPresetIds,
                                                    std::string_view const fallbackPresetId)
  {
    auto const requestedSupported =
      !requestedPresetId.empty() &&
      std::ranges::any_of(
        supportedPresetIds, [requestedPresetId](std::string_view presetId) { return presetId == requestedPresetId; });

    if (requestedSupported)
    {
      return {.presetId = std::string{requestedPresetId}, .usedFallback = false};
    }

    return {.presetId = std::string{fallbackPresetId}, .usedFallback = !requestedPresetId.empty()};
  }

  std::string LayoutSession::activeOrDefaultPresetId(std::string_view const activePresetId)
  {
    return activePresetId.empty() ? std::string{kDefaultPresetId} : std::string{activePresetId};
  }

  LayoutComponentStateDocument LayoutSession::emptyComponentState(std::string_view const presetId)
  {
    return LayoutComponentStateDocument{.preset = std::string{presetId}};
  }

  std::optional<LayoutBuildSnapshot> LayoutSession::buildSnapshot() const
  {
    return buildSnapshot(_componentState, _editMode, _onNodeMoved);
  }

  std::optional<LayoutBuildSnapshot> LayoutSession::buildSnapshot(LayoutComponentStateDocument const& componentState,
                                                                  bool const editMode,
                                                                  LayoutNodeMovedFn onNodeMoved) const
  {
    if (_generation == std::numeric_limits<std::uint64_t>::max())
    {
      return std::nullopt;
    }

    return LayoutBuildSnapshot{componentState, _generation + 1, editMode, std::move(onNodeMoved)};
  }

  ComponentStateBinding LayoutSession::stateFor(LayoutBuildSnapshot const& snapshot,
                                                LayoutSurface const surface,
                                                LayoutNode const& node,
                                                std::string_view const type)
  {
    return ComponentStateBinding{*this, snapshot, surface, node, type};
  }

  void LayoutSession::advanceGeneration(std::uint64_t const generation)
  {
    AO_EXPECTS(generation == _generation + 1, "A layout generation must commit exactly once and in order");
    _generation = generation;
  }

  void LayoutSession::apply(LayoutDocument layout,
                            LayoutComponentStateDocument componentState,
                            std::uint64_t const generation)
  {
    advanceGeneration(generation);
    _layout = std::move(layout);
    _componentState = std::move(componentState);
  }

  void LayoutSession::setEditMode(bool const editMode, LayoutNodeMovedFn onNodeMoved)
  {
    _editMode = editMode;
    _onNodeMoved = editMode ? std::move(onNodeMoved) : LayoutNodeMovedFn{};
  }

  std::optional<PanelSizePromotion> LayoutSession::preparePanelSizePromotion() const
  {
    auto promotion = PanelSizePromotion{.layout = _layout, .componentState = _componentState};
    promotion.componentState.preset = activeOrDefaultPresetId(presetId());

    if (!promotePanelSizeDefaults(promotion.layout, promotion.componentState))
    {
      return std::nullopt;
    }

    return promotion;
  }
} // namespace ao::uimodel
