// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/track/TrackCoverArt.h"
#include "layout/component/track/TrackDetail.h"
#include "layout/component/track/TrackTable.h"
#include "layout/runtime/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/UiSubscription.h"
#include "pch.h"
#include "platform/StringResources.h"
#include "track/TrackListController.h"
#include "track/TrackQuickFilterControl.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::HorizontalAlignment;
    using winrt::Microsoft::UI::Xaml::RoutedEventArgs;
    using winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox;
    using winrt::Microsoft::UI::Xaml::Controls::Button;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyout;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem;
    using winrt::Microsoft::UI::Xaml::Controls::SymbolIcon;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTipService;
    using winrt::Windows::Foundation::IInspectable;

    constexpr auto kCompactVariant = std::string_view{"compact"};

    /// A presentation id's shown name, stable across locales that do not translate it.
    std::string presentationLabel(uimodel::PresentationTextCatalog const& textCatalog,
                                  std::string_view const presentationId)
    {
      auto const optText = textCatalog.builtinTrackPresentation(presentationId);
      return stableResourceString("Presentation_", presentationId, optText ? optText->label : presentationId);
    }

    /**
     * @brief The quick filter box.
     *
     * The component owns the box and the adapter that debounces, parses, and
     * reports on it. A rejected filter expression is a transient shell message
     * like any other, so it travels through the shell rather than to whichever
     * status component happens to be in this document.
     */
    class QuickFilterComponent final : public LayoutComponent
    {
    public:
      explicit QuickFilterComponent(LayoutBuildContext& ctx)
        : _control{TrackQuickFilterControlConfig{.input = _input, .onError = ctx.reportStatus}}
      {
        _input.QueryIcon(SymbolIcon{winrt::Microsoft::UI::Xaml::Controls::Symbol::Find});
        _input.PlaceholderText(winrt::to_hstring(resourceString("QuickFilterPlaceholder")));
        _control.bind(ctx.views, ctx.workspace);
      }

      FrameworkElement element() const override { return _input; }

    private:
      AutoSuggestBox _input{};
      /// Declared last so it unbinds before the box it drives is released.
      TrackQuickFilterControl _control;
    };

    /**
     * @brief The button that names the active track presentation and switches it.
     *
     * The presentation set is a runtime fact rather than an authored one, so the
     * component builds its menu on each click from the built-in presets and the
     * eligibility of each for the active list.
     */
    class PresentationButtonComponent final : public LayoutComponent
    {
    public:
      PresentationButtonComponent(LayoutBuildContext& ctx, bool const compact)
        : _trackList{ctx.trackList}
        , _rememberPresentation{ctx.library.rememberPresentation}
        , _gatePtr{ctx.gatePtr}
        , _reportStatus{ctx.reportStatus}
      {
        _button.HorizontalContentAlignment(compact ? HorizontalAlignment::Center : HorizontalAlignment::Left);
        _buttonClickRevoker = _button.Click(winrt::auto_revoke, {this, &PresentationButtonComponent::onClicked});
        refreshLabel();
        _trackListChangedSub =
          subscribeUiUpdate(_trackList.signalChanged(), "PresentationButtonComponent", [this] { refreshLabel(); });
      }

      ~PresentationButtonComponent() override
      {
        _buttonClickRevoker.revoke();
        closeFlyout();
      }

      PresentationButtonComponent(PresentationButtonComponent const&) = delete;
      PresentationButtonComponent& operator=(PresentationButtonComponent const&) = delete;
      PresentationButtonComponent(PresentationButtonComponent&&) = delete;
      PresentationButtonComponent& operator=(PresentationButtonComponent&&) = delete;

      FrameworkElement element() const override { return _button; }

    private:
      void refreshLabel()
      {
        _button.Content(
          winrt::box_value(winrt::to_hstring(presentationLabel(_textCatalog, _trackList.activePresentationId()))));
      }

      void onClicked(IInspectable const& /*sender*/, RoutedEventArgs const& /*args*/)
      {
        if (!uimodel::isGenerationActive(_gatePtr))
        {
          return;
        }

        closeFlyout();
        _flyout = MenuFlyout{};
        auto const activeListId = _trackList.activeListId();

        for (auto const& preset : rt::builtinTrackPresentationPresets())
        {
          auto const eligibility = uimodel::trackPresentationEligibility(activeListId, preset.spec.id);
          auto item = MenuFlyoutItem{};
          item.Text(winrt::to_hstring(presentationLabel(_textCatalog, preset.spec.id)));
          item.IsEnabled(eligibility.enabled);

          if (!eligibility.enabled)
          {
            ToolTipService::SetToolTip(item, winrt::box_value(winrt::to_hstring(eligibility.disabledReason)));
          }

          _itemClickRevokers.push_back(
            item.Click(winrt::auto_revoke,
                       [this, presentationId = preset.spec.id](IInspectable const&, RoutedEventArgs const&)
                       { select(presentationId); }));
          _flyout.Items().Append(item);
        }

        _flyout.ShowAt(_button);
      }

      void closeFlyout() noexcept
      {
        _itemClickRevokers.clear();

        if (!_flyout)
        {
          return;
        }

        _flyout.Hide();

        _flyout = nullptr;
      }

      /// Switch to @p presentationId and remember it as this list's preference.
      void select(std::string const& presentationId)
      {
        if (!uimodel::isGenerationActive(_gatePtr))
        {
          return;
        }

        if (auto const selected = _trackList.selectPresentation(presentationId); !selected)
        {
          if (_reportStatus)
          {
            _reportStatus(formatResource("PresentationFailedFormat", selected.error().message));
          }

          return;
        }

        _rememberPresentation(_trackList.activeListId(), presentationId);
      }

      Button _button{};
      MenuFlyout _flyout{nullptr};
      TrackListController& _trackList;
      std::function<void(ListId, std::string)> _rememberPresentation;
      std::weak_ptr<uimodel::ShellGenerationGate> _gatePtr;
      std::function<void(std::string)> _reportStatus;
      uimodel::PresentationTextCatalog _textCatalog;
      async::Subscription _trackListChangedSub;
      Button::Click_revoker _buttonClickRevoker{};
      std::vector<MenuFlyoutItem::Click_revoker> _itemClickRevokers;
    };
  } // namespace

  void registerTrackComponents(ComponentRegistry& registry)
  {
    registry.registerComponent(
      "track.quickFilter",
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& /*node*/) -> Result<std::unique_ptr<LayoutComponent>>
      { return std::make_unique<QuickFilterComponent>(ctx); });

    registry.registerComponent(
      "track.presentationButton",
      [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        return std::make_unique<PresentationButtonComponent>(
          ctx, node.propertyOr<std::string>("variant", {}) == kCompactVariant);
      });

    registry.registerComponent("track.table",
                               [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
                               { return makeTrackTable(ctx, node); });

    registry.registerComponent("track.detail",
                               [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
                               { return makeTrackDetail(ctx, node); });

    registry.registerComponent("track.coverArt",
                               [](LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
                               { return makeTrackCoverArt(ctx, node); });
  }
} // namespace ao::winui::layout
