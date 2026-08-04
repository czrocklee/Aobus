// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/runtime/ActionBinder.h"

#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "pch.h"
#include <ao/Error.h>
#include <ao/uimodel/layout/action/LayoutActionSlot.h>
#include <ao/uimodel/layout/action/LayoutActionSlotResolution.h>
#include <ao/uimodel/layout/component/LayoutComponentActionPolicy.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>

#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <array>
#include <format>
#include <memory>
#include <string>
#include <utility>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::RoutedEventArgs;
    using winrt::Microsoft::UI::Xaml::Controls::Primitives::ButtonBase;
    using winrt::Microsoft::UI::Xaml::Input::HoldingRoutedEventArgs;
    using winrt::Microsoft::UI::Xaml::Input::RightTappedRoutedEventArgs;
    using winrt::Microsoft::UI::Xaml::Input::TappedRoutedEventArgs;
    using winrt::Windows::Foundation::IInspectable;

    constexpr auto kSlots = std::array{uimodel::LayoutActionSlot::PrimaryClick,
                                       uimodel::LayoutActionSlot::PrimaryLongPress,
                                       uimodel::LayoutActionSlot::SecondaryClick,
                                       uimodel::LayoutActionSlot::SecondaryLongPress};

    /// One bound slot, reduced to what a native event handler needs to run it.
    struct BoundAction final
    {
      ActionRegistry const* registry = nullptr;
      std::weak_ptr<uimodel::ShellGenerationGate> gatePtr;
      std::string actionId;
      uimodel::LayoutActionSlot slot = uimodel::LayoutActionSlot::PrimaryClick;

      void operator()(FrameworkElement const& anchor) const
      {
        if (!uimodel::isGenerationActive(gatePtr))
        {
          return;
        }

        registry->invoke(actionId, {.anchor = anchor, .slot = slot});
      }
    };

    void bindPrimaryClick(FrameworkElement const& element, BoundAction bound)
    {
      if (auto const button = element.try_as<ButtonBase>(); button)
      {
        button.Click([bound = std::move(bound), element](IInspectable const&, RoutedEventArgs const&)
                     { bound(element); });
        return;
      }

      element.Tapped(
        [bound = std::move(bound), element](IInspectable const&, TappedRoutedEventArgs const& args)
        {
          bound(element);
          args.Handled(true);
        });
    }

    void bindPrimaryLongPress(FrameworkElement const& element, BoundAction bound)
    {
      element.Holding(
        [bound = std::move(bound), element](IInspectable const&, HoldingRoutedEventArgs const& args)
        {
          if (args.HoldingState() != winrt::Microsoft::UI::Input::HoldingState::Completed)
          {
            return;
          }

          bound(element);
          args.Handled(true);
        });
    }

    void bindSecondaryClick(FrameworkElement const& element, BoundAction bound)
    {
      element.RightTapped(
        [bound = std::move(bound), element](IInspectable const&, RightTappedRoutedEventArgs const& args)
        {
          bound(element);
          args.Handled(true);
        });
    }
  } // namespace

  Result<> bindActions(LayoutBuildContext& ctx,
                       uimodel::LayoutNode const& node,
                       uimodel::LayoutComponentActionPolicy const& policy,
                       FrameworkElement const& element)
  {
    for (auto const slot : kSlots)
    {
      auto const optActionId = uimodel::resolveLayoutActionId(policy, node, slot);

      if (!optActionId)
      {
        continue;
      }

      auto actionId = std::string{*optActionId};

      if (!ctx.actions.contains(actionId))
      {
        return makeError(Error::Code::NotFound,
                         std::format("Node '{}' binds the unimplemented Windows action '{}'", node.id, actionId));
      }

      auto bound =
        BoundAction{.registry = &ctx.actions, .gatePtr = ctx.gatePtr, .actionId = std::move(actionId), .slot = slot};

      switch (slot)
      {
        case uimodel::LayoutActionSlot::PrimaryClick: bindPrimaryClick(element, std::move(bound)); break;
        case uimodel::LayoutActionSlot::PrimaryLongPress: bindPrimaryLongPress(element, std::move(bound)); break;
        case uimodel::LayoutActionSlot::SecondaryClick: bindSecondaryClick(element, std::move(bound)); break;
        case uimodel::LayoutActionSlot::SecondaryLongPress:
          // Windows raises one holding sequence per press regardless of button,
          // so a second long-press slot cannot be told apart from the first.
          return makeError(
            Error::Code::NotSupported,
            std::format("Node '{}' binds a secondary long press, which Windows does not raise", node.id));
      }
    }

    return {};
  }
} // namespace ao::winui::layout
