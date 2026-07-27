// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackDetailControl.h"

#include "app/LibrarySession.h"
#include "app/WinUiDependencies.h"
#include "image/CoverArtPresenter.h"
#include "platform/WindowsStringResources.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/detail/TrackCustomMetadata.h>
#include <ao/uimodel/library/detail/TrackFieldGridPolicy.h>
#include <ao/uimodel/presentation/CoverArtPlaceholder.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace ao::winui
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::GridLength;
    using winrt::Microsoft::UI::Xaml::GridUnitType;
    using winrt::Microsoft::UI::Xaml::HorizontalAlignment;
    using winrt::Microsoft::UI::Xaml::TextTrimming;
    using winrt::Microsoft::UI::Xaml::VerticalAlignment;
    using winrt::Microsoft::UI::Xaml::Visibility;
    using winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition;
    using winrt::Microsoft::UI::Xaml::Controls::FontIcon;
    using winrt::Microsoft::UI::Xaml::Controls::Grid;
    using winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
    using winrt::Microsoft::UI::Xaml::Controls::ToolTipService;

    constexpr double kDetailDisabledOpacity = 0.55;
    constexpr double kFieldLabelOpacity = 0.55;
    constexpr double kTechnicalValueOpacity = 0.72;
    constexpr double kFieldLabelWidth = 92.0;
    constexpr double kFieldRowMinimumHeight = 28.0;
    constexpr double kCompactFieldLabelWidth = 80.0;
    constexpr double kCompactFieldRowMinimumHeight = 24.0;

    constexpr std::wstring_view kChevronDownGlyph = L"\uE70D";
    constexpr std::wstring_view kChevronRightGlyph = L"\uE76C";

    std::string metadataHeading()
    {
      return resourceStringOr("InspectorMetadataHeader.Text", "Metadata");
    }

    std::string technicalHeading()
    {
      return resourceStringOr("InspectorTechnicalHeader.Text", "Audio Properties");
    }

    std::string fieldLabel(rt::TrackField const field)
    {
      return stableResourceString(
        "TrackField_", rt::trackFieldId(field), uimodel::PresentationTextCatalog{}.trackFieldLabel(field));
    }

    std::string metadataHeaderText(bool const expanded, rt::TrackDetailSnapshot const& snapshot)
    {
      if (expanded)
      {
        return metadataHeading();
      }

      auto const title =
        uimodel::formatTrackFieldDisplayText(rt::TrackField::Title, snapshot, uimodel::kMultipleTrackValuesText, true);
      auto const artist =
        uimodel::formatTrackFieldDisplayText(rt::TrackField::Artist, snapshot, uimodel::kMultipleTrackValuesText, true);
      return title.empty() && artist.empty() ? metadataHeading() : uimodel::formatMetadataHeader(title, artist);
    }

    std::string technicalHeaderText(bool const expanded, rt::TrackDetailSnapshot const& snapshot)
    {
      if (expanded)
      {
        return technicalHeading();
      }

      auto const codec = uimodel::formatTrackFieldDisplayText(rt::TrackField::Codec, snapshot, {}, false);
      auto const sampleRate = uimodel::formatTrackFieldDisplayText(rt::TrackField::SampleRate, snapshot, {}, false);
      auto const bitDepth = uimodel::formatTrackFieldDisplayText(rt::TrackField::BitDepth, snapshot, {}, false);
      return codec.empty() && sampleRate.empty() && bitDepth.empty()
               ? technicalHeading()
               : uimodel::formatTechnicalHeader(codec, sampleRate, bitDepth);
    }

    GridLength pixels(double const value) noexcept
    {
      return {.Value = value, .GridUnitType = GridUnitType::Pixel};
    }

    GridLength stars(double const value = 1.0) noexcept
    {
      return {.Value = value, .GridUnitType = GridUnitType::Star};
    }

    TextBlock makeFieldText(std::string_view const text, HorizontalAlignment const alignment, double const opacity)
    {
      auto block = TextBlock{};
      block.Text(winrt::to_hstring(text));
      block.HorizontalAlignment(alignment);
      block.VerticalAlignment(VerticalAlignment::Center);
      block.TextTrimming(TextTrimming::CharacterEllipsis);
      block.MaxLines(1);
      block.Opacity(opacity);

      if (!text.empty())
      {
        ToolTipService::SetToolTip(block, winrt::box_value(winrt::to_hstring(text)));
      }

      return block;
    }

    Grid makeFieldRow(std::string_view const label,
                      std::string_view const value,
                      bool const technical = false,
                      bool const partial = false,
                      bool const compact = false)
    {
      auto row = Grid{};
      row.MinHeight(compact ? kCompactFieldRowMinimumHeight : kFieldRowMinimumHeight);
      row.ColumnSpacing(compact ? 8.0 : 12.0);

      auto labelColumn = ColumnDefinition{};
      labelColumn.Width(pixels(compact ? kCompactFieldLabelWidth : kFieldLabelWidth));
      row.ColumnDefinitions().Append(labelColumn);

      auto valueColumn = ColumnDefinition{};
      valueColumn.Width(stars());
      row.ColumnDefinitions().Append(valueColumn);

      auto labelBlock = makeFieldText(label, HorizontalAlignment::Right, kFieldLabelOpacity);
      auto valueBlock = makeFieldText(value, HorizontalAlignment::Stretch, technical ? kTechnicalValueOpacity : 1.0);
      Grid::SetColumn(valueBlock, 1);

      if (partial)
      {
        ToolTipService::SetToolTip(
          labelBlock,
          winrt::box_value(winrt::to_hstring(resourceStringOr("MissingOnSomeTracks", "Missing on some tracks"))));
      }

      row.Children().Append(labelBlock);
      row.Children().Append(valueBlock);
      if (partial)
      {
        auto warningColumn = ColumnDefinition{};
        warningColumn.Width({.Value = 1.0, .GridUnitType = GridUnitType::Auto});
        row.ColumnDefinitions().Append(warningColumn);

        auto warning = FontIcon{};
        warning.Glyph(L"\uE7BA");
        warning.FontSize(compact ? 10.0 : 11.0);
        warning.Margin({6.0, 0.0, 0.0, 0.0});
        warning.Opacity(0.72);
        warning.VerticalAlignment(VerticalAlignment::Center);
        ToolTipService::SetToolTip(
          warning,
          winrt::box_value(winrt::to_hstring(resourceStringOr("MissingOnSomeTracks", "Missing on some tracks"))));
        Grid::SetColumn(warning, 2);
        row.Children().Append(warning);
      }
      return row;
    }

    std::string compositeValue(std::string const& primary, std::string const& secondary)
    {
      auto result = primary;
      result += " / ";
      result += secondary;
      return result;
    }

    void appendRow(StackPanel const& panel,
                   std::string_view const label,
                   std::string_view const value,
                   bool const technical = false,
                   bool const partial = false,
                   bool const compact = false)
    {
      panel.Children().Append(makeFieldRow(label, value, technical, partial, compact));
    }

    bool hasSelection(rt::TrackDetailSnapshot const& snapshot) noexcept
    {
      return snapshot.selectionKind != rt::SelectionKind::None;
    }
  } // namespace

  TrackDetailControl::TrackDetailControl(TrackDetailControlConfig config)
    : _fieldScroll{std::move(config.fieldScroll)}
    , _detailContent{std::move(config.detailContent)}
    , _metadataHeaderButton{std::move(config.metadataHeaderButton)}
    , _metadataHeader{std::move(config.metadataHeader)}
    , _metadataChevron{std::move(config.metadataChevron)}
    , _metadataRows{std::move(config.metadataRows)}
    , _showEmptyButton{std::move(config.showEmptyButton)}
    , _technicalHeaderButton{std::move(config.technicalHeaderButton)}
    , _technicalHeader{std::move(config.technicalHeader)}
    , _technicalChevron{std::move(config.technicalChevron)}
    , _technicalRows{std::move(config.technicalRows)}
    , _classicFieldScroll{std::move(config.classicFieldScroll)}
    , _classicDetailContent{std::move(config.classicDetailContent)}
    , _classicMetadataSection{std::move(config.classicMetadataSection)}
    , _classicMetadataHeaderButton{std::move(config.classicMetadataHeaderButton)}
    , _classicMetadataHeader{std::move(config.classicMetadataHeader)}
    , _classicMetadataChevron{std::move(config.classicMetadataChevron)}
    , _classicMetadataRows{std::move(config.classicMetadataRows)}
    , _classicShowEmptyButton{std::move(config.classicShowEmptyButton)}
    , _classicTechnicalSection{std::move(config.classicTechnicalSection)}
    , _classicTechnicalHeaderButton{std::move(config.classicTechnicalHeaderButton)}
    , _classicTechnicalHeader{std::move(config.classicTechnicalHeader)}
    , _classicTechnicalChevron{std::move(config.classicTechnicalChevron)}
    , _classicTechnicalRows{std::move(config.classicTechnicalRows)}
    , _schema{uimodel::buildTrackFieldGridSchema()}
  {
    if (_metadataHeaderButton)
    {
      _metadataHeaderClickRevoker = _metadataHeaderButton.Click(
        winrt::auto_revoke,
        [this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
        {
          _metadataExpanded = !_metadataExpanded;
          renderSnapshot();
        });
    }

    if (_showEmptyButton)
    {
      _showEmptyClickRevoker = _showEmptyButton.Click(
        winrt::auto_revoke,
        [this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
        {
          _showEmptyMetadata = !_showEmptyMetadata;
          renderSnapshot();
        });
    }

    if (_technicalHeaderButton)
    {
      _technicalHeaderClickRevoker = _technicalHeaderButton.Click(
        winrt::auto_revoke,
        [this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
        {
          _technicalExpanded = !_technicalExpanded;
          renderSnapshot();
        });
    }

    if (_classicMetadataHeaderButton)
    {
      _classicMetadataHeaderClickRevoker = _classicMetadataHeaderButton.Click(
        winrt::auto_revoke,
        [this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
        {
          _metadataExpanded = !_metadataExpanded;
          renderSnapshot();
        });
    }

    if (_classicShowEmptyButton)
    {
      _classicShowEmptyClickRevoker = _classicShowEmptyButton.Click(
        winrt::auto_revoke,
        [this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
        {
          _showEmptyMetadata = !_showEmptyMetadata;
          renderSnapshot();
        });
    }

    if (_classicTechnicalHeaderButton)
    {
      _classicTechnicalHeaderClickRevoker = _classicTechnicalHeaderButton.Click(
        winrt::auto_revoke,
        [this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&)
        {
          _technicalExpanded = !_technicalExpanded;
          renderSnapshot();
        });
    }

    renderSnapshot();
  }

  TrackDetailControl::~TrackDetailControl()
  {
    unbind();
    if (_classicTechnicalHeaderButton)
    {
      _classicTechnicalHeaderClickRevoker.revoke();
    }
    if (_classicShowEmptyButton)
    {
      _classicShowEmptyClickRevoker.revoke();
    }
    if (_classicMetadataHeaderButton)
    {
      _classicMetadataHeaderClickRevoker.revoke();
    }
    if (_technicalHeaderButton)
    {
      _technicalHeaderClickRevoker.revoke();
    }
    if (_showEmptyButton)
    {
      _showEmptyClickRevoker.revoke();
    }
    if (_metadataHeaderButton)
    {
      _metadataHeaderClickRevoker.revoke();
    }
  }

  void TrackDetailControl::bind(WinUiDependencies const& dependencies)
  {
    unbind();

    _runtimePtr = dependencies.session.libraryRuntimePtr();
    _coverArtPtr = &dependencies.inspectorCoverArt;
    _coverArtPtr->bind();

    try
    {
      _projectionPtr = _runtimePtr->workspace().detailProjection(rt::FocusedViewTarget{});
      _subscription = _projectionPtr->subscribe([this](rt::TrackDetailSnapshot const& snapshot) noexcept
                                                { handleSnapshot(snapshot); });
    }
    catch (...)
    {
      unbind();
      throw;
    }
  }

  void TrackDetailControl::unbind()
  {
    // TrackDetailProjection borrows services owned by the replaceable runtime.
    // Stop publication before destroying the projection and release the runtime last.
    _subscription.reset();
    _projectionPtr.reset();

    if (_coverArtPtr != nullptr)
    {
      _coverArtPtr->unbind();
      _coverArtPtr = nullptr;
    }

    _runtimePtr.reset();
    _snapshot = {};
    renderSnapshot();
    resetFieldScroll();
  }

  void TrackDetailControl::handleSnapshot(rt::TrackDetailSnapshot const& snapshot)
  {
    bool const selectionChanged = _snapshot.trackIds != snapshot.trackIds;
    _snapshot = snapshot;
    renderSnapshot();

    if (selectionChanged)
    {
      resetFieldScroll();
    }
  }

  void TrackDetailControl::renderSnapshot()
  {
    renderMetadataRows(_metadataRows, _metadataExpanded, _showEmptyMetadata, false);
    renderTechnicalRows(_technicalRows, false);
    renderMetadataRows(_classicMetadataRows, _metadataExpanded, _showEmptyMetadata, true);
    renderTechnicalRows(_classicTechnicalRows, true);
    updateSectionPresentation();
    updateSelectionPresentation();

    if (_coverArtPtr != nullptr)
    {
      auto const album = uimodel::formatTrackFieldDisplayText(rt::TrackField::Album, _snapshot, "", false);
      auto const albumArtist = uimodel::formatTrackFieldDisplayText(rt::TrackField::AlbumArtist, _snapshot, "", false);
      auto const artist = uimodel::formatTrackFieldDisplayText(rt::TrackField::Artist, _snapshot, "", false);
      auto const title = uimodel::formatTrackFieldDisplayText(rt::TrackField::Title, _snapshot, "", false);
      auto const candidates = std::array<std::string_view, 4>{album, albumArtist, artist, title};
      _coverArtPtr->select(
        _snapshot.singleCoverArtId, uimodel::makeCoverArtPlaceholderIdentity(candidates), hasSelection(_snapshot));
    }
  }

  void TrackDetailControl::renderMetadataRows(StackPanel const& rows,
                                              bool const expanded,
                                              bool const showEmpty,
                                              bool const compact)
  {
    if (!rows)
    {
      return;
    }

    rows.Children().Clear();
    for (auto const field : _schema.metadataFields)
    {
      auto const text = uimodel::formatTrackFieldDisplayText(field, _snapshot, uimodel::kMultipleTrackValuesText, true);
      auto const visible =
        uimodel::shouldShowTrackFieldGridMetadataFieldRow(uimodel::TrackFieldGridMetadataFieldVisibility{
          .metadataExpanded = expanded,
          .showEmptyMetadata = showEmpty,
          .editorEditing = false,
          .hasDisplayText = !text.empty(),
        });

      if (visible)
      {
        appendRow(rows, fieldLabel(field), text, false, false, compact);
      }
    }

    for (auto const& fields : _schema.compositeMetadataFields)
    {
      auto const primaryText =
        uimodel::formatTrackFieldDisplayText(fields.primaryField, _snapshot, uimodel::kCompositeMixedTrackText, false);
      auto const secondaryText = uimodel::formatTrackFieldDisplayText(
        fields.secondaryField, _snapshot, uimodel::kCompositeMixedTrackText, false);
      auto const visible = uimodel::shouldShowCompositeMetadataRow(uimodel::CompositeMetadataVisibility{
        .metadataExpanded = expanded,
        .showEmptyMetadata = showEmpty,
        .primaryEditorEditing = false,
        .secondaryEditorEditing = false,
        .hasPrimaryDisplayText = !primaryText.empty(),
        .hasSecondaryDisplayText = !secondaryText.empty(),
      });

      if (visible)
      {
        appendRow(
          rows, fieldLabel(fields.primaryField), compositeValue(primaryText, secondaryText), false, false, compact);
      }
    }

    for (auto const& item : _snapshot.customMetadata)
    {
      auto const text = uimodel::formatTrackCustomMetadataDisplayText(item);
      auto const visible =
        uimodel::shouldShowTrackFieldGridMetadataFieldRow(uimodel::TrackFieldGridMetadataFieldVisibility{
          .metadataExpanded = expanded,
          .showEmptyMetadata = showEmpty,
          .editorEditing = false,
          .hasDisplayText = !text.empty(),
        });

      if (visible)
      {
        appendRow(rows, item.key, text, false, !item.presentOnAll, compact);
      }
    }
  }

  void TrackDetailControl::renderTechnicalRows(StackPanel const& rows, bool const compact)
  {
    if (!rows)
    {
      return;
    }

    rows.Children().Clear();
    for (auto const field : _schema.technicalFields)
    {
      appendRow(rows,
                fieldLabel(field),
                uimodel::formatTrackFieldDisplayText(field, _snapshot, uimodel::kMultipleTrackValuesText, true),
                true,
                false,
                compact);
    }
  }

  void TrackDetailControl::updateSectionPresentation()
  {
    auto const sectionAvailability = uimodel::TrackFieldGridSectionAvailability{
      .metadataCategoryEnabled = true,
      .hasMetadataFields = !_schema.metadataFields.empty() || !_schema.compositeMetadataFields.empty(),
      .hasSelectedTracks = hasSelection(_snapshot),
      .hasTechnicalFields = !_schema.technicalFields.empty(),
    };
    auto const renderMetadataSection = uimodel::shouldRenderMetadataSection(sectionAvailability);
    auto const renderTechnicalSection = uimodel::shouldRenderTechnicalSection(sectionAvailability);

    if (_metadataHeaderButton)
    {
      _metadataHeaderButton.Visibility(renderMetadataSection ? Visibility::Visible : Visibility::Collapsed);
    }
    if (_metadataRows)
    {
      _metadataRows.Visibility(renderMetadataSection && _metadataExpanded ? Visibility::Visible
                                                                          : Visibility::Collapsed);
    }
    if (_showEmptyButton)
    {
      _showEmptyButton.Visibility(renderMetadataSection && _metadataExpanded ? Visibility::Visible
                                                                             : Visibility::Collapsed);
      _showEmptyButton.Content(winrt::box_value(
        winrt::to_hstring(_showEmptyMetadata ? resourceStringOr("HideEmptyFields", "Hide empty fields")
                                             : resourceStringOr("InspectorShowEmpty.Content", "Show empty fields"))));
    }
    if (_metadataChevron)
    {
      _metadataChevron.Glyph(winrt::hstring{_metadataExpanded ? kChevronDownGlyph : kChevronRightGlyph});
    }
    if (_metadataHeader)
    {
      _metadataHeader.Text(winrt::to_hstring(metadataHeaderText(_metadataExpanded, _snapshot)));
    }

    if (_technicalHeaderButton)
    {
      _technicalHeaderButton.Visibility(renderTechnicalSection ? Visibility::Visible : Visibility::Collapsed);
    }
    if (_technicalRows)
    {
      _technicalRows.Visibility(renderTechnicalSection && _technicalExpanded ? Visibility::Visible
                                                                             : Visibility::Collapsed);
    }
    if (_technicalChevron)
    {
      _technicalChevron.Glyph(winrt::hstring{_technicalExpanded ? kChevronDownGlyph : kChevronRightGlyph});
    }
    if (_technicalHeader)
    {
      _technicalHeader.Text(winrt::to_hstring(technicalHeaderText(_technicalExpanded, _snapshot)));
    }

    if (_classicMetadataSection)
    {
      _classicMetadataSection.Visibility(renderMetadataSection ? Visibility::Visible : Visibility::Collapsed);
    }
    if (_classicMetadataRows)
    {
      _classicMetadataRows.Visibility(_metadataExpanded ? Visibility::Visible : Visibility::Collapsed);
    }
    if (_classicShowEmptyButton)
    {
      _classicShowEmptyButton.Visibility(
        _metadataExpanded && sectionAvailability.hasMetadataFields ? Visibility::Visible : Visibility::Collapsed);
      _classicShowEmptyButton.Content(winrt::box_value(
        winrt::to_hstring(_showEmptyMetadata ? resourceStringOr("HideEmptyFields", "Hide empty fields")
                                             : resourceStringOr("InspectorShowEmpty.Content", "Show empty fields"))));
    }
    if (_classicMetadataChevron)
    {
      _classicMetadataChevron.Glyph(winrt::hstring{_metadataExpanded ? kChevronDownGlyph : kChevronRightGlyph});
    }
    if (_classicMetadataHeader)
    {
      _classicMetadataHeader.Text(winrt::to_hstring(metadataHeaderText(_metadataExpanded, _snapshot)));
    }
    if (_classicTechnicalSection)
    {
      _classicTechnicalSection.Visibility(renderTechnicalSection ? Visibility::Visible : Visibility::Collapsed);
    }
    if (_classicTechnicalRows)
    {
      _classicTechnicalRows.Visibility(_technicalExpanded ? Visibility::Visible : Visibility::Collapsed);
    }
    if (_classicTechnicalChevron)
    {
      _classicTechnicalChevron.Glyph(winrt::hstring{_technicalExpanded ? kChevronDownGlyph : kChevronRightGlyph});
    }
    if (_classicTechnicalHeader)
    {
      _classicTechnicalHeader.Text(winrt::to_hstring(technicalHeaderText(_technicalExpanded, _snapshot)));
    }
  }

  void TrackDetailControl::updateSelectionPresentation()
  {
    auto const selected = hasSelection(_snapshot);

    if (_detailContent)
    {
      _detailContent.IsHitTestVisible(selected);
      _detailContent.Opacity(selected ? 1.0 : kDetailDisabledOpacity);
    }
    if (_classicDetailContent)
    {
      _classicDetailContent.IsHitTestVisible(selected);
      _classicDetailContent.Opacity(selected ? 1.0 : kDetailDisabledOpacity);
    }
    if (_metadataHeaderButton)
    {
      _metadataHeaderButton.IsEnabled(selected);
    }
    if (_showEmptyButton)
    {
      _showEmptyButton.IsEnabled(selected);
    }
    if (_technicalHeaderButton)
    {
      _technicalHeaderButton.IsEnabled(selected);
    }
    if (_classicMetadataHeaderButton)
    {
      _classicMetadataHeaderButton.IsEnabled(selected);
    }
    if (_classicShowEmptyButton)
    {
      _classicShowEmptyButton.IsEnabled(selected);
    }
    if (_classicTechnicalHeaderButton)
    {
      _classicTechnicalHeaderButton.IsEnabled(selected);
    }
  }

  void TrackDetailControl::resetFieldScroll()
  {
    if (_fieldScroll)
    {
      _fieldScroll.ChangeView(nullptr, 0.0, nullptr, true);
    }
    if (_classicFieldScroll)
    {
      _classicFieldScroll.ChangeView(nullptr, 0.0, nullptr, true);
    }
  }
} // namespace ao::winui
