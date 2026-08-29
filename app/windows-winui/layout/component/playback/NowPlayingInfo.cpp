// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/playback/NowPlayingInfo.h"

#include "image/CoverArtPresenter.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/ResourceLookup.h"
#include "layout/runtime/UiSubscription.h"
#include "pch.h"
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>
#include <ao/winui/layout/ShellState.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.UI.Text.h>

#include <memory>
#include <string_view>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::GridLength;
    using winrt::Microsoft::UI::Xaml::GridUnitType;
    using winrt::Microsoft::UI::Xaml::TextTrimming;
    using winrt::Microsoft::UI::Xaml::Thickness;
    using winrt::Microsoft::UI::Xaml::VerticalAlignment;
    using winrt::Microsoft::UI::Xaml::Controls::Border;
    using winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::Image;
    using winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
    using winrt::Microsoft::UI::Xaml::Media::Brush;
    using winrt::Microsoft::UI::Xaml::Media::Stretch;

    constexpr auto kCoverBackgroundKey = std::string_view{"CardBackgroundFillColorSecondaryBrush"};

    constexpr double kCoverSize = 58.0;
    constexpr double kCoverColumnWidth = 62.0;
    constexpr double kCoverCornerRadius = 6.0;
    constexpr double kTextSpacing = 4.0;
    constexpr double kTextMarginLeft = 12.0;
    constexpr double kArtistFontSize = 12.0;
    constexpr double kArtistOpacity = 0.65;

    /// The cover art, title, and artist of whatever is playing.
    class NowPlayingInfoComponent final : public LayoutComponent
    {
    public:
      NowPlayingInfoComponent(LayoutBuildContext& ctx,
                              async::Runtime& asyncRuntime,
                              rt::PlaybackService& playback,
                              rt::ResourceByteMemoryCache& resourceBytes,
                              ThemeCoordinator& theme,
                              i18n::MessageCatalog const& textCatalog,
                              async::Signal<ShellState>& shellStateChanged)
        : _coverArt{_coverImage,
                    _coverPlaceholder,
                    resourceBytes,
                    theme,
                    uimodel::defaultCoverArtPlaceholderStyle(uimodel::CoverArtPlaceholderSlot::NowPlaying)}
      {
        auto coverColumn = ColumnDefinition{};
        coverColumn.Width(GridLength{.Value = kCoverColumnWidth, .GridUnitType = GridUnitType::Pixel});
        auto textColumn = ColumnDefinition{};
        textColumn.Width(GridLength{.Value = 1.0, .GridUnitType = GridUnitType::Star});
        _root.ColumnDefinitions().Append(coverColumn);
        _root.ColumnDefinitions().Append(textColumn);

        _coverImage.Stretch(Stretch::UniformToFill);
        auto coverLayers = Grid{};
        coverLayers.Children().Append(_coverPlaceholder);
        coverLayers.Children().Append(_coverImage);

        auto coverFrame = Border{};
        coverFrame.Width(kCoverSize);
        coverFrame.Height(kCoverSize);
        coverFrame.CornerRadius(winrt::Microsoft::UI::Xaml::CornerRadius{.TopLeft = kCoverCornerRadius,
                                                                         .TopRight = kCoverCornerRadius,
                                                                         .BottomRight = kCoverCornerRadius,
                                                                         .BottomLeft = kCoverCornerRadius});
        coverFrame.Child(coverLayers);

        if (auto const brush = lookupResource(ctx.resources, kCoverBackgroundKey).try_as<Brush>(); brush)
        {
          coverFrame.Background(brush);
        }

        _title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        _title.TextTrimming(TextTrimming::CharacterEllipsis);
        _artist.FontSize(kArtistFontSize);
        _artist.Opacity(kArtistOpacity);
        _artist.TextTrimming(TextTrimming::CharacterEllipsis);

        auto text = StackPanel{};
        text.Margin(Thickness{.Left = kTextMarginLeft, .Top = 0.0, .Right = 0.0, .Bottom = 0.0});
        text.Spacing(kTextSpacing);
        text.VerticalAlignment(VerticalAlignment::Center);
        text.Children().Append(_title);
        text.Children().Append(_artist);
        Grid::SetColumn(text, 1);

        _root.Children().Append(coverFrame);
        _root.Children().Append(text);

        follow(asyncRuntime, playback, textCatalog);
        applyShellState(ctx.shellState);
        _shellStateSub = subscribeUiUpdate(
          shellStateChanged, "NowPlayingInfoComponent", [this](ShellState const state) { applyShellState(state); });
      }

      FrameworkElement element() const override { return _root; }

    private:
      /**
       * @brief Give up the strip at the narrow tier.
       *
       * The Windows desktop shell specification has the Now Playing artwork and
       * text yield their space to transport, time, volume, and overflow, which
       * are the commands a narrow window still has to reach. The container gives
       * the slot back once the element is collapsed.
       */
      void applyShellState(ShellState const& state)
      {
        _root.Visibility(state.widthClass == ShellWidthClass::Narrow ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
                                                                     : winrt::Microsoft::UI::Xaml::Visibility::Visible);
      }

      void follow(async::Runtime& asyncRuntime, rt::PlaybackService& playback, i18n::MessageCatalog const& textCatalog)
      {
        _coverArt.bind(asyncRuntime);
        _viewModelPtr = std::make_unique<uimodel::NowPlayingViewModel>(
          playback, textCatalog, [this](uimodel::NowPlayingViewState const& state) { applyState(state); });
      }

      void applyState(uimodel::NowPlayingViewState const& state)
      {
        _title.Text(winrt::to_hstring(state.title));
        _artist.Text(winrt::to_hstring(state.artist));
        _coverArt.select(state.coverArtId, state.coverArtPlaceholderIdentity, true);
      }

      Grid _root{};
      Image _coverImage{};
      Grid _coverPlaceholder{};
      TextBlock _title{};
      TextBlock _artist{};
      CoverArtPresenter _coverArt;
      std::unique_ptr<uimodel::NowPlayingViewModel> _viewModelPtr;
      async::Subscription _shellStateSub;
    };
  } // namespace

  Result<std::unique_ptr<LayoutComponent>> makeNowPlayingInfo(LayoutBuildContext& ctx,
                                                              uimodel::LayoutNode const& /*node*/,
                                                              async::Runtime& asyncRuntime,
                                                              rt::PlaybackService& playback,
                                                              rt::ResourceByteMemoryCache& resourceBytes,
                                                              ThemeCoordinator& theme,
                                                              i18n::MessageCatalog const& textCatalog,
                                                              async::Signal<ShellState>& shellStateChanged)
  {
    return std::make_unique<NowPlayingInfoComponent>(
      ctx, asyncRuntime, playback, resourceBytes, theme, textCatalog, shellStateChanged);
  }
} // namespace ao::winui::layout
