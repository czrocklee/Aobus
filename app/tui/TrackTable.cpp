// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TrackTable.h"

#include "SelectableList.h"
#include "ShellInteractionModel.h"
#include "Style.h"
#include "TextCell.h"
#include "TrackListEntry.h"
#include "TrackSection.h"
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

    bool shouldRightAlignField(rt::TrackField const field)
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

    ftxui::Element columnSeparator(std::size_t const index)
    {
      return ftxui::text(index == 0 ? std::string(kColumnPadding, ' ') : std::string{"| "}) | ftxui::dim;
    }

    ftxui::Element trailingColumnSeparator()
    {
      return ftxui::text("|") | ftxui::dim;
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
        case F::Conductor: return rt::TrackFieldRawValue{std::in_place_type<std::string>, row.conductor};
        case F::Ensemble: return rt::TrackFieldRawValue{std::in_place_type<std::string>, row.ensemble};
        case F::Work: return rt::TrackFieldRawValue{std::in_place_type<std::string>, row.work};
        case F::Movement: return rt::TrackFieldRawValue{std::in_place_type<std::string>, row.movement};
        case F::Soloist: return rt::TrackFieldRawValue{std::in_place_type<std::string>, row.soloist};
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

    std::string formatFieldDisplayText(rt::TrackField const field, rt::TrackRow const& row)
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
                                      .rightAligned = shouldRightAlignField(field)});
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
        cells.push_back(columnSeparator(index));

        auto cellPtr = fieldCell(column.label, column) | style::accent() | bold;

        if (resizeHandles != nullptr)
        {
          resizeHandles->push_back(TrackColumnResizeHandle{.field = column.field, .box = {}, .columns = column.width});
          cellPtr = std::move(cellPtr) | reflect(resizeHandles->back().box);
        }

        cells.push_back(std::move(cellPtr));
      }

      if (!columns.empty())
      {
        cells.push_back(trailingColumnSeparator());
      }

      cells.push_back(filler());
      cells.push_back(fixedCell("", kScrollIndicatorColumns));
      return hbox(std::move(cells));
    }

    ftxui::Element trackRow(TrackListEntry const& track,
                            TrackId const playingTrackId,
                            std::vector<TrackColumn> const& columns)
    {
      using namespace ftxui;

      auto const playing = track.id == playingTrackId;
      auto cells = Elements{};
      cells.reserve((columns.size() * 2) + 3);
      auto playingMarkerPtr = fixedCell(playing ? std::string{">"} : std::string{}, kPlayingColumns);

      if (playing)
      {
        playingMarkerPtr = std::move(playingMarkerPtr) | style::success() | bold;
      }

      cells.push_back(std::move(playingMarkerPtr));

      for (std::size_t index = 0; index < columns.size(); ++index)
      {
        auto const& column = columns[index];
        cells.push_back(columnSeparator(index));
        cells.push_back(fieldCell(formatFieldDisplayText(column.field, track.row), column));
      }

      if (!columns.empty())
      {
        cells.push_back(trailingColumnSeparator());
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
          result.append(" · ");
        }

        result.append(details[index]);
      }

      return result;
    }

    ftxui::Element sectionHeaderRow(TrackSection const& section)
    {
      using namespace ftxui;

      auto const primary = trackSectionDisplayName(section);
      auto detail = sectionDetailText(section);

      return hbox({
        text("─ ") | dim,
        text(primary) | style::accent() | bold,
        text(detail.empty() ? std::string{} : " · " + detail) | dim,
        filler(),
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
          rows[index] = active ? rows[index] | style::selected() : rows[index] | inverted;
        }
      }

      return vbox(std::move(rows)) | focusPosition(0, std::max(0, selected)) | vscroll_indicator | frame | flex;
    }

    // A fixed-height, flexible-width empty box standing in for the off-window rows
    // above/below the built window. Preserving the total child height keeps the
    // frame scroll offset and vscroll thumb identical to a full build.
    ftxui::Element fixedHeightSpacer(std::int32_t const rows)
    {
      using namespace ftxui;

      return filler() | size(HEIGHT, EQUAL, rows);
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

  TrackTableWindow computeTrackTableWindow(std::int32_t const selectedVisualRow,
                                           std::int32_t const totalVisualRows,
                                           std::int32_t const viewportRows,
                                           std::int32_t const overscanRows) noexcept
  {
    if (totalVisualRows <= 0)
    {
      return TrackTableWindow{};
    }

    if (viewportRows <= 0)
    {
      return TrackTableWindow{
        .startVisualRow = 0, .endVisualRow = totalVisualRows, .topSpacerRows = 0, .bottomSpacerRows = 0};
    }

    // frame keeps the focused row inside the viewport, so the visible area is
    // always a subset of [anchor - viewportRows, anchor + viewportRows]. A window
    // half-height of viewportRows + overscan covers it regardless of the (never
    // read) frame scroll offset. 64-bit intermediates avoid int overflow on huge
    // lists before the clamp back into [0, totalVisualRows].
    auto const half = static_cast<std::int64_t>(viewportRows) + std::max(0, overscanRows);
    auto const anchor = static_cast<std::int64_t>(std::clamp(selectedVisualRow, 0, totalVisualRows - 1));
    auto const total = static_cast<std::int64_t>(totalVisualRows);
    auto const start = static_cast<std::int32_t>(std::clamp<std::int64_t>(anchor - half, 0, total));
    auto const end = static_cast<std::int32_t>(std::clamp<std::int64_t>(anchor + half + 1, 0, total));

    return TrackTableWindow{
      .startVisualRow = start, .endVisualRow = end, .topSpacerRows = start, .bottomSpacerRows = totalVisualRows - end};
  }

  std::vector<TrackTableRowRef> enumerateTrackTableRows(std::span<TrackSection const> const sections,
                                                        std::size_t const trackCount,
                                                        std::int32_t const startVisualRow,
                                                        std::int32_t const endVisualRow)
  {
    auto rows = std::vector<TrackTableRowRef>{};

    if (trackCount == 0)
    {
      return rows;
    }

    auto const start = std::max(0, startVisualRow);

    if (endVisualRow <= start)
    {
      return rows;
    }

    // Section header s occupies visual row (sections[s].rowBegin + s), a value
    // strictly increasing in s. Binary-search the count of headers strictly above
    // the window start; that count is the section cursor and start - it is the
    // track cursor at that point, exactly reproducing trackTableView's loop state.
    std::size_t lo = 0;
    std::size_t hi = sections.size();

    while (lo < hi)
    {
      auto const mid = lo + ((hi - lo) / 2);

      if (auto const headerVisualRow = sections[mid].rowBegin + mid; std::cmp_less(headerVisualRow, start))
      {
        lo = mid + 1;
      }
      else
      {
        hi = mid;
      }
    }

    auto sectionIndex = lo;
    auto trackIndex = static_cast<std::size_t>(start) - sectionIndex;
    auto visualRow = start;
    rows.reserve(static_cast<std::size_t>(endVisualRow - start));

    while (visualRow < endVisualRow && trackIndex < trackCount)
    {
      if (sectionIndex < sections.size() && sections[sectionIndex].rowBegin <= trackIndex)
      {
        rows.push_back(TrackTableRowRef{.isSectionHeader = true, .sectionIndex = sectionIndex, .trackIndex = 0});
        ++sectionIndex;
      }
      else
      {
        rows.push_back(TrackTableRowRef{.isSectionHeader = false, .sectionIndex = 0, .trackIndex = trackIndex});
        ++trackIndex;
      }

      ++visualRow;
    }

    return rows;
  }

  ftxui::Element trackTableView(std::span<TrackListEntry const> const tracks,
                                std::int32_t const selected,
                                TrackId const playingTrackId,
                                rt::TrackPresentationSpec const& presentation,
                                TrackTableViewOptions options)
  {
    return trackTableView(
      tracks, std::span<TrackSection const>{}, selected, playingTrackId, presentation, std::move(options));
  }

  ftxui::Element trackTableView(std::span<TrackListEntry const> const tracks,
                                std::span<TrackSection const> const sections,
                                std::int32_t const selected,
                                TrackId const playingTrackId,
                                rt::TrackPresentationSpec const& presentation,
                                TrackTableViewOptions options)
  {
    using namespace ftxui;

    auto const columns = columnsForPresentation(presentation, options.columnWidths, options.availableColumns);

    if (options.sectionRowHitRegions != nullptr)
    {
      options.sectionRowHitRegions->clear();
      options.sectionRowHitRegions->reserve(sections.size());
    }

    auto listElementPtr = ftxui::Element{};

    if (tracks.empty())
    {
      listElementPtr = selectableRows(Elements{}, -1, true, "No tracks found. Run `aobus init` in this library first.");
    }
    else
    {
      // Visual-row index equals Y pixel (every row is height 1), so windowing
      // around the selected row while padding the off-window rows with spacers
      // keeps frame/vscroll math and the focused coordinate pixel-identical to a
      // full build.
      auto const totalVisualRows = trackVisualRow(static_cast<std::int32_t>(tracks.size()) - 1, sections) + 1;
      auto const selectedVisualRow = trackVisualRow(selected, sections);
      auto const window =
        computeTrackTableWindow(selectedVisualRow, totalVisualRows, options.viewportRows, kTrackTableOverscanRows);
      auto const rowRefs = enumerateTrackTableRows(sections, tracks.size(), window.startVisualRow, window.endVisualRow);

      auto rows = Elements{};
      rows.reserve(rowRefs.size() + 2);

      if (window.topSpacerRows > 0)
      {
        rows.push_back(fixedHeightSpacer(window.topSpacerRows));
      }

      for (auto const& ref : rowRefs)
      {
        if (ref.isSectionHeader)
        {
          auto rowPtr = sectionHeaderRow(sections[ref.sectionIndex]);

          if (options.sectionRowHitRegions != nullptr)
          {
            // reflect() stores into the vector element during layout, so reserve()
            // above must keep row-box addresses stable while rows are built.
            // Off-window headers are off-screen, so emitting regions only for
            // windowed headers keeps the true sectionIndex for every clickable one.
            options.sectionRowHitRegions->push_back(
              TrackSectionRowHitRegion{.sectionIndex = static_cast<std::int32_t>(ref.sectionIndex), .box = {}});
            rowPtr = std::move(rowPtr) | reflect(options.sectionRowHitRegions->back().box);
          }

          rows.push_back(std::move(rowPtr));
        }
        else
        {
          rows.push_back(trackRow(tracks[ref.trackIndex], playingTrackId, columns));
        }
      }

      if (window.bottomSpacerRows > 0)
      {
        rows.push_back(fixedHeightSpacer(window.bottomSpacerRows));
      }

      // Highlight only when a row is selected; selected == -1 highlights nothing.
      // The selected visual row is always a built track row (never a header),
      // sitting at local offset (selectedVisualRow - startVisualRow) after the
      // optional top spacer.
      if (selected >= 0 && selectedVisualRow >= window.startVisualRow && selectedVisualRow < window.endVisualRow)
      {
        auto const rowIndex = (window.topSpacerRows > 0 ? 1 : 0) + (selectedVisualRow - window.startVisualRow);

        if (rowIndex >= 0 && std::cmp_less(rowIndex, rows.size()))
        {
          auto const index = static_cast<std::size_t>(rowIndex);
          rows[index] = std::move(rows[index]) | style::selected();
        }
      }

      listElementPtr =
        vbox(std::move(rows)) | focusPosition(0, std::max(0, selectedVisualRow)) | vscroll_indicator | frame | flex;
    }

    auto tablePtr = vbox({
                      trackHeaderRow(columns, options.resizeHandles),
                      std::move(listElementPtr),
                    }) |
                    flex;

    if (options.tableBox != nullptr)
    {
      tablePtr = std::move(tablePtr) | reflect(*options.tableBox);
    }

    return tablePtr;
  }

  std::int32_t libraryChooserPaneColumns(std::vector<std::string> const& labels, std::int32_t const terminalColumns)
  {
    auto contentColumns = std::max({cellWidth("Lists"),
                                    cellWidth("No lists found") + kScrollIndicatorColumns,
                                    cellWidth(overlayHint(Overlay::ListChooser))});

    for (auto const& label : labels)
    {
      contentColumns = std::max(contentColumns, cellWidth(label) + kScrollIndicatorColumns);
    }

    return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element libraryChooserPane(std::vector<std::string> const& labels,
                                    std::int32_t const selected,
                                    std::int32_t columns)
  {
    using namespace ftxui;

    if (columns <= 0)
    {
      columns = libraryChooserPaneColumns(labels, 0);
    }

    auto rows = std::vector<SelectableListRow>{};
    rows.reserve(labels.size());

    for (std::size_t index = 0; index < labels.size(); ++index)
    {
      rows.push_back(
        SelectableListRow{.elementPtr = text(labels[index]) | flex, .selected = std::cmp_equal(index, selected)});
    }

    return style::popupPanel("Lists",
                             vbox({
                               selectableList(std::move(rows),
                                              SelectableListOptions{.focusRow = std::max(0, selected),
                                                                    .emptyText = "No lists found",
                                                                    .framed = !labels.empty(),
                                                                    .scrollIndicator = !labels.empty(),
                                                                    .flex = !labels.empty(),
                                                                    .centerEmpty = labels.empty()}),
                               separator(),
                               style::panelFooterHint(overlayHint(Overlay::ListChooser)),
                             })) |
           size(WIDTH, EQUAL, columns);
  }
} // namespace ao::tui
