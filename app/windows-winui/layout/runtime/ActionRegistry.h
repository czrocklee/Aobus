// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/utility/TransparentStringHash.h>

#include <boost/unordered/unordered_flat_map.hpp>
#include <winrt/Microsoft.UI.Xaml.h>

#include <functional>
#include <string>
#include <string_view>

namespace ao::winui::layout
{
  /// Where an action was invoked from, for actions that present from an anchor.
  struct ActionContext final
  {
    winrt::Microsoft::UI::Xaml::FrameworkElement anchor{nullptr};
    uimodel::LayoutActionSlot slot = uimodel::LayoutActionSlot::PrimaryClick;
  };

  using ActionHandler = std::function<void(ActionContext const&)>;

  /**
   * @brief Shell-lifetime map from a catalog action id to its native handler.
   *
   * An action names a shell capability, not a widget, so the registry outlives
   * every view generation. Components reference only ids, which is what lets a
   * generation be replaced without re-registering behavior.
   */
  class ActionRegistry final
  {
  public:
    void registerAction(std::string_view id, ActionHandler handler);

    bool contains(std::string_view id) const;

    /// Invoke @p id if it is registered, and report whether a handler ran.
    bool invoke(std::string_view id, ActionContext const& context) const;

  private:
    boost::
      unordered_flat_map<std::string, ActionHandler, utility::TransparentStringHash, utility::TransparentStringEqual>
        _handlers;
  };
} // namespace ao::winui::layout
