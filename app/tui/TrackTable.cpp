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
#include <ao/uimodel/library/presentation/TrackColumnWidthSolver.h>
#include <ao/uimodel/library/presentation/TrackFieldPresentationPolicy.h>

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
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
    constexpr std::int32_t kMaximumFieldColumns = 72;
    constexpr std::int32_t kScrollIndicatorColumns = 1;
    // GTK policy widths are pixels; TUI columns are character cells. This keeps
    // relative presentation widths while avoiding terminal-specific measurement.
    constexpr std::int32_t kPresentationPixelToTerminalColumnRatio = 12;

    struct TrackColumn final
    {
      rt::TrackField field = rt::TrackField::Title;
      std::string label{};
      std::int32_t width = kMinimumTrackColumnWidthColumns;
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
        return kMinimumTrackColumnWidthColumns;
      }

      return std::clamp(
        policyWidth / kPresentationPixelToTerminalColumnRatio, kMinimumTrackColumnWidthColumns, kMaximumFieldColumns);
    }

    std::int32_t terminalMinimumColumnWidth(rt::TrackField const field)
    {
      return std::clamp(uimodel::minimumTrackFieldColumnWidth(field) / kPresentationPixelToTerminalColumnRatio,
                        kMinimumTrackColumnWidthColumns,
                        kMaximumFieldColumns);
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
            std::in_place_type<std::string>,
            uimodel::formatTechnicalSummary(row.codec, row.sampleRate, row.bitDepth, row.bitrate)};
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

    std::optional<std::int32_t> overrideColumnWidth(rt::TrackField const field,
                                                    std::vector<TrackColumnWidthOverride> const* const columnWidths)
    {
      if (columnWidths == nullptr)
      {
        return std::nullopt;
      }

      auto const it = std::ranges::find(*columnWidths, field, &TrackColumnWidthOverride::field);

      if (it == columnWidths->end() || it->columns <= 0)
      {
        return std::nullopt;
      }

      return std::clamp(it->columns, kMinimumTrackColumnWidthColumns, kMaximumTrackColumnResizeColumns);
    }

    std::int32_t trackColumnViewport(std::int32_t const availableColumns, std::size_t const columnCount)
    {
      if (availableColumns <= 0 || columnCount == 0)
      {
        return 0;
      }

      auto const separators = columnCount > 1 ? static_cast<std::int32_t>(columnCount - 1) * 2 : 0;
      auto const trailingSeparator = columnCount > 0 ? 1 : 0;
      auto const chromeColumns =
        kPlayingColumns + kColumnPadding + separators + trailingSeparator + kScrollIndicatorColumns;
      return std::max(0, availableColumns - chromeColumns);
    }

    std::vector<TrackColumn> columnsForPresentation(rt::TrackPresentationSpec const& presentation,
                                                    std::vector<TrackColumnWidthOverride> const* const columnWidths,
                                                    std::int32_t const availableColumns)
    {
      auto normalized = rt::normalizeTrackPresentationSpec(presentation);

      if (normalized.visibleFields.empty())
      {
        normalized = rt::defaultTrackPresentationSpec();
      }

      auto columns = std::vector<TrackColumn>{};
      columns.reserve(normalized.visibleFields.size());
      auto specs = std::vector<uimodel::TrackColumnSolveSpec>{};
      specs.reserve(normalized.visibleFields.size());

      for (auto const field : normalized.visibleFields)
      {
        auto spec = uimodel::TrackColumnSolveSpec{.field = field,
                                                  .weight = uimodel::defaultTrackFieldColumnWeight(field),
                                                  .fixedWidth = -1,
                                                  .defaultWidth = terminalColumnWidth(field),
                                                  .minimumWidth = terminalMinimumColumnWidth(field)};

        if (auto const optOverrideWidth = overrideColumnWidth(field, columnWidths); optOverrideWidth)
        {
          spec.fixedWidth = *optOverrideWidth;
        }

        specs.push_back(spec);
      }

      auto const widths =
        uimodel::solveTrackColumnWidths(specs, trackColumnViewport(availableColumns, normalized.visibleFields.size()));

      for (std::size_t index = 0; index < normalized.visibleFields.size(); ++index)
      {
        auto const field = normalized.visibleFields[index];
        auto const width = index < widths.size() ? widths[index] : terminalColumnWidth(field);
        columns.push_back(TrackColumn{.field = field,
                                      .label = std::string{uimodel::trackFieldColumnTitle(field)},
                                      .width = width,
                                      .rightAligned = rightAlignField(field)});
      }

      return columns;
    }

    ftxui::Element trackHeaderRow(std::vector<TrackColumn> const& columns,
                                  std::vector<TrackColumnResizeHandle>* const resizeHandles)
    {
      using namespace ftxui;

      auto cells = Elements{};
      cells.reserve((columns.size() * 2) + 4);
      cells.push_back(fixedCell("", kPlayingColumns));

      if (resizeHandles != nullptr)
      {
        resizeHandles->clear();
        resizeHandles->reserve(columns.size());
      }

      for (std::size_t index = 0; index < columns.size(); ++index)
      {
        auto const& column = columns[index];
        cells.push_back(text(index == 0 ? std::string(kColumnPadding, ' ') : std::string{"| "}));

        auto cellPtr = fieldCell(column.label, column);

        if (resizeHandles != nullptr)
        {
          resizeHandles->push_back(TrackColumnResizeHandle{.field = column.field, .box = {}, .columns = column.width});
          cellPtr = std::move(cellPtr) | reflect(resizeHandles->back().box);
        }

        cells.push_back(std::move(cellPtr));
      }

      if (!columns.empty())
      {
        cells.push_back(text("|"));
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
      cells.reserve((columns.size() * 2) + 3);
      cells.push_back(fixedCell(playing ? std::string{">"} : std::string{}, kPlayingColumns));

      for (std::size_t index = 0; index < columns.size(); ++index)
      {
        auto const& column = columns[index];
        cells.push_back(text(index == 0 ? std::string(kColumnPadding, ' ') : std::string{"| "}));
        cells.push_back(fieldCell(displayTextForField(column.field, track.row), column));
      }

      if (!columns.empty())
      {
        cells.push_back(text("|"));
      }

      cells.push_back(filler());
      return hbox(std::move(cells));
    }

    std::string sectionDetailText(TrackSection const& section)
    {
      auto details = std::vector<std::string>{};

      if (!section.secondaryText.empty())
      {
        details.push_back(section.secondaryText);
      }

      if (!section.tertiaryText.empty())
      {
        details.push_back(section.tertiaryText);
      }

      details.push_back(
        std::format("{} {}", section.rowCount, section.rowCount == std::size_t{1} ? "track" : "tracks"));

      auto result = std::string{};

      for (std::size_t index = 0; index < details.size(); ++index)
      {
        if (index > 0)
        {
          result.append("  ");
        }

        result.append(details[index]);
      }

      return result;
    }

    ftxui::Element sectionHeaderRow(TrackSection const& section)
    {
      using namespace ftxui;

      auto const primary = sectionDisplayName(section);
      auto detail = sectionDetailText(section);

      return hbox({
        text("  "),
        text(primary) | bold,
        filler(),
        text(std::move(detail)) | dim,
        text(" "),
      });
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

  std::int32_t trackVisualRow(std::int32_t const trackIndex, std::span<TrackSection const> const sections) noexcept
  {
    if (trackIndex < 0)
    {
      return -1;
    }

    std::int32_t headerCount = 0;

    for (auto const& section : sections)
    {
      if (std::cmp_less_equal(section.rowBegin, trackIndex))
      {
        ++headerCount;
      }
    }

    return trackIndex + headerCount;
  }

  std::int32_t trackIndexForVisualRow(std::int32_t const visualRow,
                                      std::size_t const trackCount,
                                      std::span<TrackSection const> const sections) noexcept
  {
    if (trackCount == 0)
    {
      return 0;
    }

    auto const maxVisualRow = static_cast<std::int32_t>(std::min<std::size_t>(
      trackCount + sections.size() - 1, static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())));
    auto const clampedVisualRow = std::clamp(visualRow, 0, maxVisualRow);
    std::int32_t headersBefore = 0;

    for (auto const& section : sections)
    {
      auto const headerRow = static_cast<std::int32_t>(section.rowBegin) + headersBefore;

      if (clampedVisualRow == headerRow)
      {
        return static_cast<std::int32_t>(std::min(section.rowBegin, trackCount - 1));
      }

      if (clampedVisualRow < headerRow)
      {
        break;
      }

      ++headersBefore;
    }

    auto const trackIndex = clampedVisualRow - headersBefore;
    auto const maxTrackIndex = static_cast<std::int32_t>(
      std::min<std::size_t>(trackCount - 1, static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())));
    return std::clamp(trackIndex, 0, maxTrackIndex);
  }

  ftxui::Element trackTableView(std::span<TrackListItem const> const tracks,
                                std::int32_t const selected,
                                TrackId const playingTrackId,
                                rt::TrackPresentationSpec const& presentation,
                                TrackTableViewOptions options)
  {
    return trackTableView(
      tracks, std::span<TrackSection const>{}, selected, playingTrackId, presentation, std::move(options));
  }

  ftxui::Element trackTableView(std::span<TrackListItem const> const tracks,
                                std::span<TrackSection const> const sections,
                                std::int32_t const selected,
                                TrackId const playingTrackId,
                                rt::TrackPresentationSpec const& presentation,
                                TrackTableViewOptions options)
  {
    using namespace ftxui;

    auto const columns = columnsForPresentation(presentation, options.columnWidths, options.availableColumns);
    auto rows = Elements{};
    rows.reserve(tracks.size() + sections.size());
    std::size_t sectionIndex = 0;

    if (options.sectionRowBoxes != nullptr)
    {
      options.sectionRowBoxes->clear();
      options.sectionRowBoxes->reserve(sections.size());
    }

    for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex)
    {
      while (sectionIndex < sections.size() && sections[sectionIndex].rowBegin <= trackIndex)
      {
        auto rowPtr = sectionHeaderRow(sections[sectionIndex]);

        if (options.sectionRowBoxes != nullptr)
        {
          // reflect() stores into the vector element during layout, so reserve()
          // above must keep row-box addresses stable while rows are built.
          options.sectionRowBoxes->push_back(
            TrackSectionRowBox{.sectionIndex = static_cast<std::int32_t>(sectionIndex), .box = {}});
          rowPtr = std::move(rowPtr) | reflect(options.sectionRowBoxes->back().box);
        }

        rows.push_back(std::move(rowPtr));
        ++sectionIndex;
      }

      rows.push_back(trackRow(tracks[trackIndex], playingTrackId, columns));
    }

    auto const visualSelected = trackVisualRow(selected, sections);
    auto tablePtr =
      vbox({
        trackHeaderRow(columns, options.resizeHandles),
        selectableRows(
          std::move(rows), visualSelected, true, "No tracks found. Run `aobus init` in this library first."),
      }) |
      flex;

    if (options.tableBox != nullptr)
    {
      tablePtr = std::move(tablePtr) | reflect(*options.tableBox);
    }

    return tablePtr;
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
