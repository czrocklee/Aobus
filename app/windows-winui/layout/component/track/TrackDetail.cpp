// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/track/TrackDetail.h"

#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/ResourceLookup.h"
#include "pch.h"
#include "track/TrackDetailControl.h"
#include <ao/Error.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::FrameworkElement;
    using winrt::Microsoft::UI::Xaml::HorizontalAlignment;
    using winrt::Microsoft::UI::Xaml::ResourceDictionary;
    using winrt::Microsoft::UI::Xaml::Thickness;
    using winrt::Microsoft::UI::Xaml::VerticalAlignment;
    using winrt::Microsoft::UI::Xaml::Visibility;
    using winrt::Microsoft::UI::Xaml::Controls::Border;
    using winrt::Microsoft::UI::Xaml::Controls::Button;
    using winrt::Microsoft::UI::Xaml::Controls::FontIcon;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::Orientation;
    using winrt::Microsoft::UI::Xaml::Controls::ScrollBarVisibility;
    using winrt::Microsoft::UI::Xaml::Controls::ScrollViewer;
    using winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
    using winrt::Microsoft::UI::Xaml::Media::Brush;
    using winrt::Microsoft::UI::Xaml::Media::SolidColorBrush;
    using winrt::Microsoft::UI::Xaml::Shapes::Rectangle;

    constexpr auto kSectionHeaderStyleKey = std::string_view{"InspectorSectionHeaderStyle"};
    constexpr auto kDividerBrushKey = std::string_view{"DividerStrokeColorDefaultBrush"};
    constexpr auto kCardBrushKey = std::string_view{"CardBackgroundFillColorDefaultBrush"};
    constexpr auto kAccentTextBrushKey = std::string_view{"AccentTextFillColorPrimaryBrush"};

    constexpr double kSectionSpacing = 4.0;
    constexpr double kHeaderLabelSpacing = 8.0;
    constexpr double kHeaderFontSize = 11.0;
    constexpr double kChevronFontSize = 10.0;
    constexpr double kHeaderCharacterSpacing = 80.0;
    constexpr double kDividerHeight = 1.0;
    constexpr double kHeaderBackdropMarginLeft = 2.0;
    constexpr double kShowEmptyButtonHorizontalPadding = 8.0;
    constexpr double kShowEmptyButtonVerticalPadding = 4.0;
    constexpr double kContentMargin = 16.0;

    /// The elements one collapsible detail section is made of.
    struct DetailSection final
    {
      Button headerButton{nullptr};
      FontIcon chevron{nullptr};
      TextBlock label{nullptr};
      StackPanel rows{nullptr};
    };

    /**
     * @brief A section heading: a rule across the region with the title punched into it.
     *
     * The label sits on the card background so the divider appears to run behind
     * it, which is why the heading needs the same brush the inspector card uses.
     */
    DetailSection buildSection(ResourceDictionary const& resources)
    {
      auto section = DetailSection{
        .headerButton = Button{},
        .chevron = FontIcon{},
        .label = TextBlock{},
        .rows = StackPanel{},
      };

      auto divider = Rectangle{};
      divider.Height(kDividerHeight);
      divider.VerticalAlignment(VerticalAlignment::Center);

      if (auto const brush = lookupResource(resources, kDividerBrushKey).try_as<Brush>(); brush)
      {
        divider.Fill(brush);
      }

      section.chevron.FontSize(kChevronFontSize);
      section.label.FontSize(kHeaderFontSize);
      section.label.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
      section.label.CharacterSpacing(static_cast<std::int32_t>(kHeaderCharacterSpacing));

      auto labelRow = StackPanel{};
      labelRow.Orientation(Orientation::Horizontal);
      labelRow.Spacing(kHeaderLabelSpacing);
      labelRow.Children().Append(section.chevron);
      labelRow.Children().Append(section.label);

      auto labelBackdrop = Border{};
      labelBackdrop.Margin(Thickness{.Left = kHeaderBackdropMarginLeft, .Top = 0.0, .Right = 0.0, .Bottom = 0.0});
      labelBackdrop.Padding(Thickness{.Left = 0.0, .Top = 0.0, .Right = kHeaderLabelSpacing, .Bottom = 0.0});
      labelBackdrop.HorizontalAlignment(HorizontalAlignment::Left);
      labelBackdrop.Child(labelRow);

      if (auto const brush = lookupResource(resources, kCardBrushKey).try_as<Brush>(); brush)
      {
        labelBackdrop.Background(brush);
      }

      auto heading = Grid{};
      heading.Children().Append(divider);
      heading.Children().Append(labelBackdrop);

      section.headerButton.Content(heading);

      if (auto const style =
            lookupResource(resources, kSectionHeaderStyleKey).try_as<winrt::Microsoft::UI::Xaml::Style>();
          style)
      {
        section.headerButton.Style(style);
      }

      section.rows.Spacing(kSectionSpacing);
      return section;
    }

    /// The scrolling detail region the inspector shows for the focused selection.
    class TrackDetailComponent final : public LayoutComponent
    {
    public:
      TrackDetailComponent(LayoutBuildContext& ctx,
                           rt::WorkspaceService& workspace,
                           i18n::MessageCatalog const& textCatalog)
        : _metadata{buildSection(ctx.resources)}
        , _technical{buildSection(ctx.resources)}
        , _focusedDetailPtr{ctx.focusedDetailPtr}
      {
        _showEmptyButton.HorizontalAlignment(HorizontalAlignment::Left);
        _showEmptyButton.Background(SolidColorBrush{winrt::Windows::UI::Colors::Transparent()});
        _showEmptyButton.BorderThickness(Thickness{});
        _showEmptyButton.Padding(Thickness{.Left = kShowEmptyButtonHorizontalPadding,
                                           .Top = kShowEmptyButtonVerticalPadding,
                                           .Right = kShowEmptyButtonHorizontalPadding,
                                           .Bottom = kShowEmptyButtonVerticalPadding});

        if (auto const brush = lookupResource(ctx.resources, kAccentTextBrushKey).try_as<Brush>(); brush)
        {
          _showEmptyButton.Foreground(brush);
        }

        _technical.headerButton.Margin(Thickness{.Left = 0.0, .Top = kSectionSpacing, .Right = 0.0, .Bottom = 0.0});
        // The audio properties start collapsed; the adapter reveals them.
        _technical.rows.Visibility(Visibility::Collapsed);

        _content.Margin(
          Thickness{.Left = kContentMargin, .Top = 0.0, .Right = kContentMargin, .Bottom = kContentMargin});
        _content.Spacing(kSectionSpacing);
        _content.Children().Append(_metadata.headerButton);
        _content.Children().Append(_metadata.rows);
        _content.Children().Append(_showEmptyButton);
        _content.Children().Append(_technical.headerButton);
        _content.Children().Append(_technical.rows);

        _scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        _scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        _scroll.Content(_content);

        // Built last, and only once every element it drives is arranged the way
        // this component wants it: the adapter renders as soon as it exists, and
        // section visibility is its decision from that moment on.
        _controlPtr = std::make_unique<TrackDetailControl>(TrackDetailControlConfig{
          .fieldScroll = _scroll,
          .detailContent = _content,
          .metadataHeaderButton = _metadata.headerButton,
          .metadataHeader = _metadata.label,
          .metadataChevron = _metadata.chevron,
          .metadataRows = _metadata.rows,
          .showEmptyButton = _showEmptyButton,
          .technicalHeaderButton = _technical.headerButton,
          .technicalHeader = _technical.label,
          .technicalChevron = _technical.chevron,
          .technicalRows = _technical.rows,
          .textCatalog = textCatalog,
        });
        follow(workspace);
      }

      FrameworkElement element() const override { return _scroll; }

    private:
      void follow(rt::WorkspaceService& workspace)
      {
        // The cover is `track.coverArt`, wherever the document put it, so the
        // detail region reads the same shared projection and draws no artwork.
        _controlPtr->bind({.projectionPtr = _focusedDetailPtr->projection(workspace)});
      }

      ScrollViewer _scroll{};
      StackPanel _content{};
      DetailSection _metadata;
      DetailSection _technical;
      Button _showEmptyButton{};
      std::shared_ptr<FocusedDetail> _focusedDetailPtr;
      /// Declared last so it releases its projection before the elements it drives.
      std::unique_ptr<TrackDetailControl> _controlPtr;
    };
  } // namespace

  Result<std::unique_ptr<LayoutComponent>> makeTrackDetail(LayoutBuildContext& ctx,
                                                           uimodel::LayoutNode const& /*node*/,
                                                           rt::WorkspaceService& workspace,
                                                           i18n::MessageCatalog const& textCatalog)
  {
    return std::make_unique<TrackDetailComponent>(ctx, workspace, textCatalog);
  }
} // namespace ao::winui::layout
