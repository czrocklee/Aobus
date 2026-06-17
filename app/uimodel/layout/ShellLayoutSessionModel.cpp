// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/LayoutComponentState.h>
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutStatePromoter.h>
#include <ao/uimodel/layout/ShellLayoutSessionModel.h>

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace ao::uimodel::layout
{
  LayoutPresetSelection ShellLayoutSessionModel::selectPreset(std::string_view requestedPresetId,
                                                              std::span<std::string_view const> supportedPresetIds,
                                                              std::string_view fallbackPresetId)
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

  std::string ShellLayoutSessionModel::activeOrDefaultPresetId(std::string_view activePresetId)
  {
    return activePresetId.empty() ? std::string{kDefaultPresetId} : std::string{activePresetId};
  }

  LayoutComponentStateDocument ShellLayoutSessionModel::emptyComponentState(std::string_view presetId)
  {
    return LayoutComponentStateDocument{.preset = std::string{presetId}};
  }

  ShellLayoutSessionSnapshot ShellLayoutSessionModel::snapshot() const
  {
    return {.presetId = _activePresetId, .layout = _activeLayout};
  }

  void ShellLayoutSessionModel::applyLoadedLayout(std::string presetId, LayoutDocument layout)
  {
    _activePresetId = std::move(presetId);
    _activeLayout = std::move(layout);
  }

  void ShellLayoutSessionModel::applyEditorSave(std::string presetId, LayoutDocument layout)
  {
    applyLoadedLayout(std::move(presetId), std::move(layout));
  }

  ShellLayoutRuntimeStateReset ShellLayoutSessionModel::resetRuntimeLayoutState()
  {
    auto const presetId = activeOrDefaultPresetId(_activePresetId);
    auto componentState = emptyComponentState(presetId);

    _activePresetId = presetId;

    return {.presetId = presetId, .componentState = std::move(componentState)};
  }

  std::optional<ShellLayoutPanelSizePromotion> ShellLayoutSessionModel::preparePanelSizePromotion(
    LayoutComponentStateDocument const& runtimeComponentState) const
  {
    auto promotion = ShellLayoutPanelSizePromotion{.presetId = activeOrDefaultPresetId(_activePresetId),
                                                   .layout = _activeLayout,
                                                   .componentState = runtimeComponentState,
                                                   .result = {}};
    promotion.componentState.preset = promotion.presetId;
    promotion.result = promotePanelSizeDefaults(promotion.layout, promotion.componentState);

    if (!promotion.result.changed)
    {
      return std::nullopt;
    }

    return promotion;
  }

  void ShellLayoutSessionModel::applyPanelSizePromotion(ShellLayoutPanelSizePromotion promotion)
  {
    _activePresetId = std::move(promotion.presetId);
    _activeLayout = std::move(promotion.layout);
  }
} // namespace ao::uimodel::layout
