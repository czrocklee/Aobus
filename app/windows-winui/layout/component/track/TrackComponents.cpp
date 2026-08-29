// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/track/TrackCoverArt.h"
#include "layout/component/track/TrackDetail.h"
#include "layout/runtime/ComponentRegistrations.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "pch.h"
#include "platform/StringResources.h"
#include "track/TrackListController.h"
#include "track/TrackQuickFilterControl.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>
#include <ao/uimodel/library/list/ListActions.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/presentation/TrackPresentationPickerViewModel.h>
#include <ao/uimodel/library/track/TrackFilterView.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
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
    using winrt::Microsoft::UI::Xaml::Visibility;
    using winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox;
    using winrt::Microsoft::UI::Xaml::Controls::Button;
    using winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyout;
    using winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem;
    using winrt::Microsoft::UI::Xaml::Controls::SymbolIcon;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTipService;
    using winrt::Windows::Foundation::IInspectable;

    constexpr auto kCompactVariant = std::string_view{"compact"};
    constexpr double kQuickFilterColumnSpacing = 8.0;

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
      QuickFilterComponent(LayoutBuildContext& ctx,
                           std::function<void(ListId, std::string)> createList,
                           TrackListController& trackList,
                           rt::ViewService& views,
                           rt::WorkspaceService& workspace,
                           rt::CompletionService& completion,
                           i18n::MessageCatalog const& textCatalog,
                           std::function<void(std::string)> reportStatus)
        : _createList{std::move(createList)}, _trackList{trackList}, _gatePtr{ctx.gatePtr}
      {
        auto inputColumn = ColumnDefinition{};
        inputColumn.Width(winrt::Microsoft::UI::Xaml::GridLength{
          .Value = 1.0,
          .GridUnitType = winrt::Microsoft::UI::Xaml::GridUnitType::Star,
        });
        auto buttonColumn = ColumnDefinition{};
        buttonColumn.Width(winrt::Microsoft::UI::Xaml::GridLength{
          .Value = 0.0,
          .GridUnitType = winrt::Microsoft::UI::Xaml::GridUnitType::Auto,
        });
        _root.ColumnDefinitions().Append(inputColumn);
        _root.ColumnDefinitions().Append(buttonColumn);
        _root.ColumnSpacing(kQuickFilterColumnSpacing);
        _input.QueryIcon(SymbolIcon{winrt::Microsoft::UI::Xaml::Controls::Symbol::Find});
        _input.PlaceholderText(winrt::to_hstring(resourceString("winui_library_quick_filter_placeholder")));
        _root.Children().Append(_input);

        auto const createLabel = i18n::requiredText(textCatalog, i18n::MessageId::WinUiListCreateFromFilter);
        _createButton.Content(winrt::box_value(winrt::to_hstring(createLabel)));
        _createButton.Visibility(Visibility::Collapsed);
        ToolTipService::SetToolTip(_createButton, winrt::box_value(winrt::to_hstring(createLabel)));
        _createClickRevoker = _createButton.Click(
          winrt::auto_revoke,
          [this](IInspectable const&, RoutedEventArgs const&)
          {
            if (_createList && !_resolvedExpression.empty() && uimodel::isGenerationActive(_gatePtr))
            {
              _createList(uimodel::parentForNewSmartList(_trackList.activeListId()), _resolvedExpression);
            }
          });
        Grid::SetColumn(_createButton, 1);
        _root.Children().Append(_createButton);

        // Construction publishes the current filter state synchronously, so
        // all native chrome must be in place first.
        _control.emplace(
          TrackQuickFilterControlConfig{
            .input = _input,
            .onError = std::move(reportStatus),
            .onState =
              [this](uimodel::TrackFilterViewState const& state)
            {
              _resolvedExpression = state.resolvedExpression;
              _createButton.Visibility(state.canCreateSmartList ? Visibility::Visible : Visibility::Collapsed);
              _createButton.IsEnabled(state.canCreateSmartList);
            },
            .textCatalog = textCatalog,
          },
          views,
          workspace,
          completion);
      }

      FrameworkElement element() const override { return _root; }

    private:
      Grid _root{};
      AutoSuggestBox _input{};
      Button _createButton{};
      std::function<void(ListId, std::string)> _createList;
      TrackListController& _trackList;
      std::weak_ptr<uimodel::ShellGenerationGate> _gatePtr;
      std::string _resolvedExpression;
      Button::Click_revoker _createClickRevoker{};
      /// Declared last so it stops before the box it drives is released.
      std::optional<TrackQuickFilterControl> _control;
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
      PresentationButtonComponent(LayoutBuildContext& ctx,
                                  TrackListController& trackList,
                                  rt::ViewService& views,
                                  rt::WorkspaceService& workspace,
                                  uimodel::TrackPresentationCatalog& presentationCatalog,
                                  uimodel::ListPresentations& listPresentations,
                                  i18n::MessageCatalog const& textCatalog,
                                  std::function<void(std::string)> reportStatus,
                                  bool const compact)
        : _trackList{trackList}
        , _gatePtr{ctx.gatePtr}
        , _reportStatus{std::move(reportStatus)}
        , _viewModelPtr{std::make_unique<uimodel::TrackPresentationPickerViewModel>(
            views,
            workspace,
            presentationCatalog,
            listPresentations,
            textCatalog,
            [this](uimodel::TrackPresentationPickerState const& state)
            {
              _state = state;
              refreshLabel();
            })}
      {
        _button.HorizontalContentAlignment(compact ? HorizontalAlignment::Center : HorizontalAlignment::Left);
        _buttonClickRevoker = _button.Click(winrt::auto_revoke, {this, &PresentationButtonComponent::onClicked});
        _viewModelPtr->refresh();
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
        _button.Content(winrt::box_value(winrt::to_hstring(_state.label)));
        _button.IsEnabled(_state.enabled);
      }

      void onClicked(IInspectable const& /*sender*/, RoutedEventArgs const& /*args*/)
      {
        if (!uimodel::isGenerationActive(_gatePtr))
        {
          return;
        }

        closeFlyout();
        _flyout = MenuFlyout{};

        for (std::size_t index = 0; index < _state.menuItems.size(); ++index)
        {
          auto const& entry = _state.menuItems[index];

          if (entry.type == uimodel::TrackPresentationMenuItemType::CreateCustomView)
          {
            continue;
          }

          if (entry.type == uimodel::TrackPresentationMenuItemType::Separator)
          {
            auto const hasLaterPreset = std::ranges::any_of(
              _state.menuItems.begin() + static_cast<std::ptrdiff_t>(index + 1),
              _state.menuItems.end(),
              [](auto const& candidate) { return candidate.type == uimodel::TrackPresentationMenuItemType::Preset; });

            if (hasLaterPreset)
            {
              _flyout.Items().Append(winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator{});
            }

            continue;
          }

          auto item = MenuFlyoutItem{};
          item.Text(winrt::to_hstring(entry.label));
          item.IsEnabled(entry.enabled);

          if (!entry.enabled)
          {
            ToolTipService::SetToolTip(item, winrt::box_value(winrt::to_hstring(entry.disabledReason)));
          }

          _itemClickRevokers.push_back(
            item.Click(winrt::auto_revoke,
                       [this, presentationId = entry.id](IInspectable const&, RoutedEventArgs const&)
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

        auto const optSelection = _viewModelPtr->selectPresentation(presentationId);

        if (!optSelection)
        {
          return;
        }

        if (auto const selectedRes = _trackList.selectPresentation(optSelection->spec); !selectedRes)
        {
          if (_reportStatus)
          {
            _reportStatus(formatResource("winui_presentation_failed", selectedRes.error().message));
          }

          return;
        }

        _viewModelPtr->completeSelection(*optSelection);
      }

      Button _button{};
      MenuFlyout _flyout{nullptr};
      TrackListController& _trackList;
      std::weak_ptr<uimodel::ShellGenerationGate> _gatePtr;
      std::function<void(std::string)> _reportStatus;
      uimodel::TrackPresentationPickerState _state;
      std::unique_ptr<uimodel::TrackPresentationPickerViewModel> _viewModelPtr;
      Button::Click_revoker _buttonClickRevoker{};
      std::vector<MenuFlyoutItem::Click_revoker> _itemClickRevokers;
    };
  } // namespace

  void registerTrackComponents(ComponentRegistry& registry,
                               async::Runtime& asyncRuntime,
                               rt::ViewService& views,
                               rt::WorkspaceService& workspace,
                               rt::CompletionService& completion,
                               rt::ResourceByteMemoryCache& resourceBytes,
                               ThemeCoordinator& theme,
                               TrackListController& trackList,
                               uimodel::TrackPresentationCatalog& presentationCatalog,
                               uimodel::ListPresentations& listPresentations,
                               std::function<void(ListId, std::string)> createList,
                               i18n::MessageCatalog textCatalog,
                               std::function<void(std::string)> reportStatus)
  {
    registry.registerComponent(
      "track.quickFilter",
      [createList = std::move(createList), &trackList, &views, &workspace, &completion, textCatalog, reportStatus](
        LayoutBuildContext& ctx, uimodel::LayoutNode const& /*node*/) -> Result<std::unique_ptr<LayoutComponent>>
      {
        return std::make_unique<QuickFilterComponent>(
          ctx, createList, trackList, views, workspace, completion, textCatalog, reportStatus);
      });

    registry.registerComponent(
      "track.presentationButton",
      [&trackList, &views, &workspace, &presentationCatalog, &listPresentations, textCatalog, reportStatus](
        LayoutBuildContext& ctx, uimodel::LayoutNode const& node) -> Result<std::unique_ptr<LayoutComponent>>
      {
        return std::make_unique<PresentationButtonComponent>(
          ctx,
          trackList,
          views,
          workspace,
          presentationCatalog,
          listPresentations,
          textCatalog,
          reportStatus,
          node.propertyOr<std::string>("variant", {}) == kCompactVariant);
      });

    registry.registerComponent("track.detail",
                               [&workspace, textCatalog](LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
                               { return makeTrackDetail(ctx, node, workspace, textCatalog); });

    registry.registerComponent(
      "track.coverArt",
      [&asyncRuntime, &workspace, &resourceBytes, &theme, textCatalog](
        LayoutBuildContext& ctx, uimodel::LayoutNode const& node)
      { return makeTrackCoverArt(ctx, node, asyncRuntime, workspace, resourceBytes, theme, textCatalog); });
  }
} // namespace ao::winui::layout
