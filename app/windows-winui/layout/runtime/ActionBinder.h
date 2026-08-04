// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <winrt/Microsoft.UI.Xaml.h>

namespace ao::uimodel
{
  struct LayoutComponentActionPolicy;
  struct LayoutNode;
} // namespace ao::uimodel

namespace ao::winui::layout
{
  struct LayoutBuildContext;

  /**
   * @brief Wire every action slot @p node resolves under @p policy onto @p element.
   *
   * Slot mapping follows what the Windows shell already does natively: a primary
   * click is a button's `Click` and a plain tap elsewhere, a secondary click is
   * `RightTapped`, and a primary long press is a completed `Holding`. Handlers
   * capture the generation weakly, so a pointer sequence that finishes after the
   * generation is retired invokes nothing.
   *
   * An action id that no handler is registered for fails the whole candidate:
   * a document naming a capability the shell does not provide is a shipped
   * artifact defect, not something to discover when the user clicks.
   */
  Result<> bindActions(LayoutBuildContext& ctx,
                       uimodel::LayoutNode const& node,
                       uimodel::LayoutComponentActionPolicy const& policy,
                       winrt::Microsoft::UI::Xaml::FrameworkElement const& element);
} // namespace ao::winui::layout
