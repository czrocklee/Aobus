// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackDetailControl.h"

#include "platform/StringResources.h"
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/rt/projection/TrackDetailSnapshot.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/detail/TrackCustomMetadata.h>
#include <ao/uimodel/library/detail/TrackFieldGridPolicy.h>
#include <ao/uimodel/library/detail/TrackFieldGridSchema.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

#include <string>
#include <string_view>
#include <utility>

namespace ao::winui
{
  using i18n::MessageId;
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
    constexpr double kFieldColumnSpacing = 12.0;
    constexpr double kWarningIconSize = 11.0;
    constexpr double kWarningIconLeftMargin = 6.0;
    constexpr double kWarningIconOpacity = 0.72;

    constexpr std::wstring_view kChevronDownGlyph = L"\uE70D";
    constexpr std::wstring_view kChevronRightGlyph = L"\uE76C";

    std::string metadataHeading(uimodel::PresentationTextCatalog const& textCatalog)
    {
      return std::string{textCatalog.text(MessageId::TrackMetadataHeading)};
    }

    std::string technicalHeading(uimodel::PresentationTextCatalog const& textCatalog)
    {
      return std::string{textCatalog.text(MessageId::TrackAudioPropertiesHeading)};
    }

    std::string fieldLabel(uimodel::PresentationTextCatalog const& textCatalog, rt::TrackField const field)
    {
      return stableResourceString("track_field_", rt::trackFieldId(field), textCatalog.trackFieldLabel(field));
    }

    std::string metadataHeaderText(uimodel::PresentationTextCatalog const& textCatalog,
                                   bool const expanded,
                                   rt::TrackDetailSnapshot const& snapshot)
    {
      if (expanded)
      {
        return metadataHeading(textCatalog);
      }

      auto const title = uimodel::formatTrackFieldDisplayText(
        textCatalog, rt::TrackField::Title, snapshot, textCatalog.text(MessageId::TrackMultipleValues), true);
      auto const artist = uimodel::formatTrackFieldDisplayText(
        textCatalog, rt::TrackField::Artist, snapshot, textCatalog.text(MessageId::TrackMultipleValues), true);
      return title.empty() && artist.empty() ? metadataHeading(textCatalog)
                                             : uimodel::formatMetadataHeader(title, artist);
    }

    std::string technicalHeaderText(uimodel::PresentationTextCatalog const& textCatalog,
                                    bool const expanded,
                                    rt::TrackDetailSnapshot const& snapshot)
    {
      if (expanded)
      {
        return technicalHeading(textCatalog);
      }

      auto const codec = uimodel::formatTrackFieldDisplayText(textCatalog, rt::TrackField::Codec, snapshot, {}, false);
      auto const sampleRate =
        uimodel::formatTrackFieldDisplayText(textCatalog, rt::TrackField::SampleRate, snapshot, {}, false);
      auto const bitDepth =
        uimodel::formatTrackFieldDisplayText(textCatalog, rt::TrackField::BitDepth, snapshot, {}, false);
      return codec.empty() && sampleRate.empty() && bitDepth.empty()
               ? technicalHeading(textCatalog)
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
                      std::string_view const missingText = {})
    {
      auto row = Grid{};
      row.MinHeight(kFieldRowMinimumHeight);
      row.ColumnSpacing(kFieldColumnSpacing);

      auto labelColumn = ColumnDefinition{};
      labelColumn.Width(pixels(kFieldLabelWidth));
      row.ColumnDefinitions().Append(labelColumn);

      auto valueColumn = ColumnDefinition{};
      valueColumn.Width(stars());
      row.ColumnDefinitions().Append(valueColumn);

      auto labelBlock = makeFieldText(label, HorizontalAlignment::Right, kFieldLabelOpacity);
      auto valueBlock = makeFieldText(value, HorizontalAlignment::Stretch, technical ? kTechnicalValueOpacity : 1.0);
      Grid::SetColumn(valueBlock, 1);

      if (partial)
      {
        ToolTipService::SetToolTip(labelBlock, winrt::box_value(winrt::to_hstring(missingText)));
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
        warning.FontSize(kWarningIconSize);
        warning.Margin({
          .Left = kWarningIconLeftMargin,
          .Top = 0.0,
          .Right = 0.0,
          .Bottom = 0.0,
        });
        warning.Opacity(kWarningIconOpacity);
        warning.VerticalAlignment(VerticalAlignment::Center);
        ToolTipService::SetToolTip(warning, winrt::box_value(winrt::to_hstring(missingText)));
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
                   std::string_view const missingText = {})
    {
      panel.Children().Append(makeFieldRow(label, value, technical, partial, missingText));
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
    , _textCatalog{std::move(config.textCatalog)}
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

    renderSnapshot();
  }

  TrackDetailControl::~TrackDetailControl()
  {
    unbind();

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

  void TrackDetailControl::bind(TrackDetailBinding binding)
  {
    unbind();
    resetPresentation();

    try
    {
      _projectionPtr = std::move(binding.projectionPtr);
      _subscription =
        _projectionPtr->subscribe([this](rt::TrackDetailSnapshot const& snapshot) { handleSnapshot(snapshot); });
    }
    catch (...)
    {
      unbind();
      throw;
    }
  }

  void TrackDetailControl::unbind() noexcept
  {
    // TrackDetailProjection borrows services owned by this window's runtime.
    // Stop publication before destroying the projection.
    _subscription.reset();

    _projectionPtr.reset();
    _snapshot = {};
  }

  void TrackDetailControl::resetPresentation()
  {
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
    renderMetadataRows(_metadataRows, _metadataExpanded, _showEmptyMetadata);
    renderTechnicalRows(_technicalRows);
    updateSectionPresentation();
    updateSelectionPresentation();
  }

  void TrackDetailControl::renderMetadataRows(StackPanel const& rows, bool const expanded, bool const showEmpty)
  {
    if (!rows)
    {
      return;
    }

    rows.Children().Clear();

    for (auto const field : _schema.metadataFields)
    {
      auto const text = uimodel::formatTrackFieldDisplayText(
        _textCatalog, field, _snapshot, _textCatalog.text(MessageId::TrackMultipleValues), true);
      auto const visible =
        uimodel::shouldShowTrackFieldGridMetadataFieldRow(uimodel::TrackFieldGridMetadataFieldVisibility{
          .metadataExpanded = expanded,
          .showEmptyMetadata = showEmpty,
          .editorEditing = false,
          .hasDisplayText = !text.empty(),
        });

      if (visible)
      {
        appendRow(rows, fieldLabel(_textCatalog, field), text);
      }
    }

    for (auto const& fields : _schema.compositeMetadataFields)
    {
      auto const primaryText = uimodel::formatTrackFieldDisplayText(
        _textCatalog, fields.primaryField, _snapshot, uimodel::kCompositeMixedTrackText, false);
      auto const secondaryText = uimodel::formatTrackFieldDisplayText(
        _textCatalog, fields.secondaryField, _snapshot, uimodel::kCompositeMixedTrackText, false);
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
        appendRow(rows, fieldLabel(_textCatalog, fields.primaryField), compositeValue(primaryText, secondaryText));
      }
    }

    for (auto const& item : _snapshot.customMetadata)
    {
      auto const text =
        uimodel::formatTrackCustomMetadataDisplayText(item, _textCatalog.text(MessageId::TrackMultipleValues));
      auto const visible =
        uimodel::shouldShowTrackFieldGridMetadataFieldRow(uimodel::TrackFieldGridMetadataFieldVisibility{
          .metadataExpanded = expanded,
          .showEmptyMetadata = showEmpty,
          .editorEditing = false,
          .hasDisplayText = !text.empty(),
        });

      if (visible)
      {
        appendRow(rows, item.key, text, false, !item.presentOnAll, _textCatalog.text(MessageId::TrackMissingOnSome));
      }
    }
  }

  void TrackDetailControl::renderTechnicalRows(StackPanel const& rows)
  {
    if (!rows)
    {
      return;
    }

    rows.Children().Clear();

    for (auto const field : _schema.technicalFields)
    {
      appendRow(rows,
                fieldLabel(_textCatalog, field),
                uimodel::formatTrackFieldDisplayText(
                  _textCatalog, field, _snapshot, _textCatalog.text(MessageId::TrackMultipleValues), true),
                true);
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
    applySectionPresentation(renderMetadataSection, renderTechnicalSection);
  }

  void TrackDetailControl::applySectionPresentation(bool const renderMetadataSection, bool const renderTechnicalSection)
  {
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
      _showEmptyButton.Content(winrt::box_value(winrt::to_hstring(
        _textCatalog.text(_showEmptyMetadata ? MessageId::TrackHideEmptyFields : MessageId::TrackShowEmptyFields))));
    }

    if (_metadataChevron)
    {
      _metadataChevron.Glyph(winrt::hstring{_metadataExpanded ? kChevronDownGlyph : kChevronRightGlyph});
    }

    if (_metadataHeader)
    {
      _metadataHeader.Text(winrt::to_hstring(metadataHeaderText(_textCatalog, _metadataExpanded, _snapshot)));
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
      _technicalHeader.Text(winrt::to_hstring(technicalHeaderText(_textCatalog, _technicalExpanded, _snapshot)));
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
  }

  void TrackDetailControl::resetFieldScroll()
  {
    if (_fieldScroll)
    {
      _fieldScroll.ChangeView(nullptr, 0.0, nullptr, true);
    }
  }
} // namespace ao::winui
