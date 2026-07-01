// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackTable.h"

#include "Model.h"
#include "ShellModel.h"
#include "TextCell.h"
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutPolicy.h>
#include <ao/uimodel/library/presentation/TrackFieldPresentationPolicy.h>

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    constexpr std::int32_t kPlayingColumns = 2;
    constexpr std::int32_t kColumnPadding = 2;
    constexpr std::int32_t kMinimumFieldColumns = 8;
    constexpr std::int32_t kMaximumFieldColumns = 72;
    constexpr std::int32_t kExpandedFieldColumns = 72;
    constexpr std::int32_t kScrollIndicatorColumns = 1;
    // GTK policy widths are pixels; TUI columns are character cells. This keeps
    // relative presentation widths while avoiding terminal-specific measurement.
    constexpr std::int32_t kPresentationPixelToTerminalColumnRatio = 12;

    struct TrackColumn final
    {
      rt::TrackField field = rt::TrackField::Title;
      std::string label{};
      std::int32_t width = kMinimumFieldColumns;
      bool rightAligned = false;
    };

    std::string blankFallback(std::string_view value)
    {
      return value.empty() ? std::string{"-"} : std::string{value};
    }

    bool rightAlignField(rt::TrackField const field)
    {
      using F = rt::TrackField;

      switch (field)
      {
        case F::Year:
        case F::DiscNumber:
        case F::DiscTotal:
        case F::TrackNumber:
        case F::TrackTotal:
        case F::MovementNumber:
        case F::MovementTotal:
        case F::Duration:
        case F::SampleRate:
        case F::Channels:
        case F::BitDepth:
        case F::Bitrate:
        case F::FileSize:
        case F::ModifiedTime:
        case F::DisplayTrackNumber: return true;
        default: return false;
      }
    }

    std::int32_t terminalColumnWidth(rt::TrackField const field)
    {
      auto const policyWidth = uimodel::defaultTrackFieldColumnWidth(field);

      if (policyWidth <= 0)
      {
        return kMinimumFieldColumns;
      }

      return std::clamp(
        policyWidth / kPresentationPixelToTerminalColumnRatio, kMinimumFieldColumns, kMaximumFieldColumns);
    }

    ftxui::Element fixedCell(std::string value, std::int32_t const width, bool const rightAligned = false)
    {
      return ftxui::text(fitCellText(value, width, rightAligned ? CellAlignment::Right : CellAlignment::Left)) |
             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, width);
    }

    ftxui::Element fieldCell(std::string value, TrackColumn const& column)
    {
      return fixedCell(std::move(value), column.width, column.rightAligned);
    }

    rt::TrackFieldRawValue rawValueForField(rt::TrackField const field, rt::TrackRow const& row)
    {
      using F = rt::TrackField;

      switch (field)
      {
        case F::Title: return rt::TrackFieldRawValue{std::in_place_type<std::string>, trackDisplayTitle(row)};
        case F::Artist: return rt::TrackFieldRawValue{std::in_place_type<std::string>, row.artist};
        case F::Album: return rt::TrackFieldRawValue{std::in_place_type<std::string>, row.album};
        case F::AlbumArtist: return rt::TrackFieldRawValue{std::in_place_type<std::string>, row.albumArtist};
        case F::Genre: return rt::TrackFieldRawValue{std::in_place_type<std::string>, row.genre};
        case F::Composer: return rt::TrackFieldRawValue{std::in_place_type<std::string>, row.composer};
        case F::Work: return rt::TrackFieldRawValue{std::in_place_type<std::string>, row.work};
        case F::Movement: return rt::TrackFieldRawValue{std::in_place_type<std::string>, row.movement};
        case F::Year: return rt::TrackFieldRawValue{std::in_place_type<std::uint16_t>, row.year};
        case F::DiscNumber: return rt::TrackFieldRawValue{std::in_place_type<std::uint16_t>, row.discNumber};
        case F::DiscTotal: return rt::TrackFieldRawValue{std::in_place_type<std::uint16_t>, row.discTotal};
        case F::TrackNumber: return rt::TrackFieldRawValue{std::in_place_type<std::uint16_t>, row.trackNumber};
        case F::TrackTotal: return rt::TrackFieldRawValue{std::in_place_type<std::uint16_t>, row.trackTotal};
        case F::MovementNumber: return rt::TrackFieldRawValue{std::in_place_type<std::uint16_t>, row.movementNumber};
        case F::MovementTotal: return rt::TrackFieldRawValue{std::in_place_type<std::uint16_t>, row.movementTotal};
        case F::Duration: return rt::TrackFieldRawValue{std::in_place_type<rt::TrackFieldDuration>, row.duration};
        case F::Tags: return rt::TrackFieldRawValue{std::in_place_type<std::string>, row.tags};
        case F::FilePath:
          return rt::TrackFieldRawValue{
            std::in_place_type<std::string>, row.optUriPath ? row.optUriPath->string() : std::string{}};
        case F::Codec: return rt::TrackFieldRawValue{std::in_place_type<std::string>, uimodel::formatCodec(row.codec)};
        case F::SampleRate: return rt::TrackFieldRawValue{std::in_place_type<std::uint32_t>, row.sampleRate};
        case F::Channels: return rt::TrackFieldRawValue{std::in_place_type<std::uint32_t>, row.channels};
        case F::BitDepth: return rt::TrackFieldRawValue{std::in_place_type<std::uint32_t>, row.bitDepth};
        case F::Bitrate: return rt::TrackFieldRawValue{std::in_place_type<std::uint32_t>, row.bitrate};
        case F::FileSize: return rt::TrackFieldRawValue{std::in_place_type<std::uint64_t>, row.fileSize};
        case F::ModifiedTime: return rt::TrackFieldRawValue{std::in_place_type<std::uint64_t>, row.modifiedTime};
        case F::DisplayTrackNumber:
          return rt::TrackFieldRawValue{
            std::in_place_type<std::string>,
            uimodel::formatDisplayTrackNumber(row.discNumber, row.discTotal, row.trackNumber)};
        case F::TechnicalSummary:
          return rt::TrackFieldRawValue{
            std::in_place_type<std::string>, uimodel::formatTechnicalSummary(row.codec, row.sampleRate, row.bitDepth)};
        case F::Quality: return rt::TrackFieldRawValue{};
      }

      return rt::TrackFieldRawValue{};
    }

    std::string displayTextForField(rt::TrackField const field, rt::TrackRow const& row)
    {
      auto value = uimodel::formatTrackFieldRawValue(field, rawValueForField(field, row));

      if (field == rt::TrackField::Duration && value.empty())
      {
        return "--:--";
      }

      if (field == rt::TrackField::DisplayTrackNumber && value.empty())
      {
        return "--";
      }

      return blankFallback(value);
    }

    std::vector<TrackColumn> columnsForPresentation(rt::TrackPresentationSpec const& presentation)
    {
      auto normalized = rt::normalizeTrackPresentationSpec(presentation);

      if (normalized.visibleFields.empty())
      {
        normalized = rt::defaultTrackPresentationSpec();
      }

      auto columns = std::vector<TrackColumn>{};
      columns.reserve(normalized.visibleFields.size());
      auto const expandingField = uimodel::expandingTrackColumn(normalized.visibleFields);

      for (auto const field : normalized.visibleFields)
      {
        auto width = terminalColumnWidth(field);

        if (field == expandingField)
        {
          width = std::max(width, kExpandedFieldColumns);
        }

        columns.push_back(TrackColumn{.field = field,
                                      .label = std::string{uimodel::trackFieldColumnTitle(field)},
                                      .width = width,
                                      .rightAligned = rightAlignField(field)});
      }

      return columns;
    }

    ftxui::Element trackHeaderRow(std::vector<TrackColumn> const& columns)
    {
      using namespace ftxui;

      auto cells = Elements{};
      cells.reserve((columns.size() * 2) + 1);
      cells.push_back(fixedCell("", kPlayingColumns));

      for (auto const& column : columns)
      {
        cells.push_back(text(std::string(kColumnPadding, ' ')));
        cells.push_back(fieldCell(column.label, column));
      }

      cells.push_back(filler());
      cells.push_back(fixedCell("", kScrollIndicatorColumns));
      return hbox(std::move(cells)) | dim;
    }

    ftxui::Element trackRow(TrackListItem const& track,
                            TrackId const playingTrackId,
                            std::vector<TrackColumn> const& columns)
    {
      using namespace ftxui;

      auto const playing = track.id == playingTrackId;
      auto cells = Elements{};
      cells.reserve((columns.size() * 2) + 1);
      cells.push_back(fixedCell(playing ? std::string{">"} : std::string{}, kPlayingColumns));

      for (auto const& column : columns)
      {
        cells.push_back(text(std::string(kColumnPadding, ' ')));
        cells.push_back(fieldCell(displayTextForField(column.field, track.row), column));
      }

      cells.push_back(filler());
      return hbox(std::move(cells));
    }

    ftxui::Element selectableRows(ftxui::Elements rows,
                                  std::int32_t const selected,
                                  bool const active,
                                  std::string const& emptyText)
    {
      using namespace ftxui;

      if (rows.empty())
      {
        return vbox({text(emptyText) | dim}) | center;
      }

      for (std::size_t index = 0; index < rows.size(); ++index)
      {
        if (std::cmp_equal(index, selected))
        {
          rows[index] = rows[index] | inverted;

          if (active)
          {
            rows[index] = rows[index] | bold;
          }
        }
      }

      return vbox(std::move(rows)) | focusPosition(0, std::max(0, selected)) | vscroll_indicator | frame | flex;
    }
  } // namespace

  ftxui::Element trackTableView(std::vector<TrackListItem> const& tracks,
                                std::int32_t const selected,
                                TrackId const playingTrackId,
                                rt::TrackPresentationSpec const& presentation)
  {
    using namespace ftxui;

    auto columns = columnsForPresentation(presentation);
    auto rows = Elements{};
    rows.reserve(tracks.size());

    for (auto const& track : tracks)
    {
      rows.push_back(trackRow(track, playingTrackId, columns));
    }

    return vbox({
             trackHeaderRow(columns),
             selectableRows(
               std::move(rows), selected, true, "No tracks found. Run `aobus init` in this library first."),
           }) |
           flex;
  }

  ftxui::Element libraryChooserPane(std::vector<std::string> const& labels, std::int32_t const selected)
  {
    using namespace ftxui;

    auto rows = Elements{};
    rows.reserve(labels.size());

    for (auto const& label : labels)
    {
      rows.push_back(text(label) | flex);
    }

    return vbox({
             text("Lists") | bold,
             separator(),
             selectableRows(std::move(rows), selected, true, "No lists found"),
             separator(),
             text(std::string{overlayHint(Overlay::ListChooser)}) | dim,
           }) |
           border | size(WIDTH, EQUAL, kLibraryChooserPaneColumns);
  }
} // namespace ao::tui
