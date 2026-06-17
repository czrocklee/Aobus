// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/LayoutComponentState.h>
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutStatePromoter.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::uimodel::layout
{
  struct LayoutPresetSelection final
  {
    std::string presetId;
    bool usedFallback = false;
  };

  struct ShellLayoutRuntimeStateReset final
  {
    std::string presetId;
    LayoutComponentStateDocument componentState;
  };

  struct ShellLayoutPanelSizePromotion final
  {
    std::string presetId;
    LayoutDocument layout;
    LayoutComponentStateDocument componentState;
    PanelSizePromotionResult result;
  };

  struct ShellLayoutSessionSnapshot final
  {
    std::string const& presetId;
    LayoutDocument const& layout;
  };

  class ShellLayoutSessionModel final
  {
  public:
    static constexpr std::string_view kDefaultPresetId = "classic";

    static LayoutPresetSelection selectPreset(std::string_view requestedPresetId,
                                              std::span<std::string_view const> supportedPresetIds,
                                              std::string_view fallbackPresetId = kDefaultPresetId);
    static std::string activeOrDefaultPresetId(std::string_view activePresetId);
    static LayoutComponentStateDocument emptyComponentState(std::string_view presetId);

    ShellLayoutSessionSnapshot snapshot() const;

    void applyLoadedLayout(std::string presetId, LayoutDocument layout);
    void applyEditorSave(std::string presetId, LayoutDocument layout);

    ShellLayoutRuntimeStateReset resetRuntimeLayoutState();
    std::optional<ShellLayoutPanelSizePromotion> preparePanelSizePromotion(
      LayoutComponentStateDocument const& runtimeComponentState) const;
    void applyPanelSizePromotion(ShellLayoutPanelSizePromotion promotion);

  private:
    std::string _activePresetId;
    LayoutDocument _activeLayout;
  };
} // namespace ao::uimodel::layout
