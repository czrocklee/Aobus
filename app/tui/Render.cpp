// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Render.h"

#include "CoverArt.h"
#include "ShellInteractionModel.h"
#include "Style.h"
#include "TextCell.h"
#include "TrackDetailLines.h"
#include "TrackListEntry.h"
#include "TuiTextCatalog.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/presentation/PresentationText.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    /**
     * @brief The cells a field label may claim before it is shortened.
     *
     * The GTK detail grid caps its key column the same way and for the same
     * reason: one long label in one locale must not spend the pane's width on
     * naming a field instead of showing its value.
     */
    constexpr std::int32_t kDetailLabelColumns = 12;
    /// The value budget the pane reserves, so its width never follows a track.
    constexpr std::int32_t kDetailValueColumns = 24;
    /// The value budget Detail keeps once labels have taken their share.
    constexpr std::int32_t kMinimumDetailValueColumns = 12;
    /// The share of the body labels may claim before values start paying.
    constexpr std::int32_t kDetailLabelPercent = 40;
    constexpr std::string_view kDetailLabelDelimiter = ": ";
    /// Frame edges and the artwork separator: the rows Detail spends on chrome.
    constexpr std::int32_t kDetailChromeRows = 3;

    constexpr auto kHelpPaneLines = std::to_array<i18n::MessageId>({
      i18n::MessageId::TuiShellHelpQuickFilter,
      i18n::MessageId::TuiShellHelpChooseList,
      i18n::MessageId::TuiShellHelpTrackDetail,
      i18n::MessageId::TuiShellHelpAudioPipeline,
      i18n::MessageId::TuiShellHelpOutputDevice,
      i18n::MessageId::TuiShellHelpChooseView,
      i18n::MessageId::TuiShellHelpNotifications,
      i18n::MessageId::TuiShellHelpCurrentTrack,
      i18n::MessageId::TuiShellHelpSwitchPresentation,
      i18n::MessageId::TuiShellHelpPreviousNextGroup,
      i18n::MessageId::TuiShellHelpClearFilter,
      i18n::MessageId::TuiShellHelpReloadList,
      i18n::MessageId::TuiShellHelpPlayback,
      i18n::MessageId::TuiShellHelpQuit,
      i18n::MessageId::TuiShellHelpFooter,
    });
    constexpr std::size_t kHelpPaneFooterLine = kHelpPaneLines.size() - 1;
    constexpr std::size_t kHelpPaneSeparatorCount = 2;
    using ResolvedHelpPaneLines = std::array<std::string, kHelpPaneLines.size()>;

    ResolvedHelpPaneLines resolveHelpPaneLines(i18n::MessageCatalog const& textCatalog)
    {
      auto result = ResolvedHelpPaneLines{};

      for (std::size_t index = 0; index < kHelpPaneLines.size(); ++index)
      {
        result[index] = tuiChromeText(textCatalog, kHelpPaneLines[index]);
      }

      return result;
    }

    std::int32_t resolvedHelpPaneColumns(ResolvedHelpPaneLines const& lines,
                                         std::string_view const title,
                                         std::int32_t const terminalColumns)
    {
      std::int32_t contentColumns = cellWidth(title);

      for (auto const& line : lines)
      {
        contentColumns = std::max(contentColumns, cellWidth(line));
      }

      return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
    }

    ftxui::Element popoverClearHalo(ftxui::Element popoverPtr)
    {
      return std::move(popoverPtr) | ftxui::borderEmpty | ftxui::clear_under;
    }

    /**
     * @brief DEC private mode 2026, which holds the terminal's rendering until
     *        the end escape arrives.
     *
     * Writing a delete and its replacement draw in one call still leaves the
     * terminal free to render between parsing them, because it renders on its
     * own clock rather than per write. Bracketing the pair is what actually
     * makes it one frame. A terminal without the mode ignores it and sees the
     * two escapes back to back, which is the behavior without this bracket.
     */
    constexpr std::string_view kSynchronizedUpdateBegin = "\033[?2026h";
    constexpr std::string_view kSynchronizedUpdateEnd = "\033[?2026l";

    /// The cells Kitty paints into, held open by an element that draws nothing.
    ftxui::Element kittyCoverArtReservation(std::int32_t const columns)
    {
      using namespace ftxui;

      return text("") | size(WIDTH, EQUAL, columns) | size(HEIGHT, EQUAL, kCoverArtRows);
    }

    /// The label column this locale asks for, capped and delimiter included.
    std::int32_t detailLabelContentColumns(i18n::MessageCatalog const& textCatalog)
    {
      std::int32_t labelColumns = 0;

      for (auto const field : trackDetailFields())
      {
        labelColumns = std::max(labelColumns, cellWidth(uimodel::trackFieldLabel(textCatalog, field)));
      }

      return std::min(labelColumns, kDetailLabelColumns) + cellWidth(kDetailLabelDelimiter);
    }

    struct DetailBodySplit final
    {
      std::int32_t labelColumns = 0;
      std::int32_t valueColumns = 0;
    };

    /**
     * @brief How @p bodyColumns is divided between labels and values.
     *
     * Labels ask for the widest they can ever be, then give way twice: they
     * never take more than their share of the body, and never take so much
     * that values fall below what a value needs to say anything.
     */
    DetailBodySplit detailBodySplit(i18n::MessageCatalog const& textCatalog, std::int32_t const bodyColumns)
    {
      auto const labelShare = bodyColumns * kDetailLabelPercent / 100;
      auto const valueFloor = bodyColumns - kMinimumDetailValueColumns;
      auto const labelColumns = std::max(0, std::min({detailLabelContentColumns(textCatalog), labelShare, valueFloor}));

      return {.labelColumns = labelColumns, .valueColumns = std::max(0, bodyColumns - labelColumns)};
    }

    std::string detailLabelText(std::string_view const label, std::int32_t const labelColumns)
    {
      auto const delimiterColumns = cellWidth(kDetailLabelDelimiter);

      if (labelColumns <= delimiterColumns)
      {
        return ellipsizeToCellWidth(label, labelColumns);
      }

      auto labelText = ellipsizeToCellWidth(label, labelColumns - delimiterColumns);
      labelText.append(kDetailLabelDelimiter);
      return labelText;
    }
  } // namespace

  void defaultKittyEscapeSink(std::string_view const escapeSequence)
  {
    std::print("{}", escapeSequence);
    std::fflush(stdout);
  }

  bool isValidBox(ftxui::Box const& box)
  {
    return box.x_max > box.x_min && box.y_max > box.y_min;
  }

  bool isSameBox(ftxui::Box const& left, ftxui::Box const& right)
  {
    return left.x_min == right.x_min && left.x_max == right.x_max && left.y_min == right.y_min &&
           left.y_max == right.y_max;
  }

  bool isSameKittyImage(KittyPaintState const& state, ResourceId const coverArtId, ftxui::Box const& coverBox)
  {
    return state.visible && coverArtId == state.paintedCoverArtId && isSameBox(coverBox, state.paintedCoverBox);
  }

  ftxui::Element detailCoverArt(i18n::MessageCatalog const& textCatalog,
                                CoverArtDeliveryMode const mode,
                                std::optional<CoverArtRows> const& optPreview,
                                std::optional<std::vector<std::byte>> const& optKittyPng,
                                std::int32_t const columns,
                                ftxui::Box* const optArtworkBox)
  {
    using namespace ftxui;

    if (optArtworkBox != nullptr)
    {
      *optArtworkBox = Box{};
    }

    if (mode == CoverArtDeliveryMode::Off)
    {
      return {};
    }

    auto artworkPtr = Element{};

    if (mode == CoverArtDeliveryMode::Kitty)
    {
      artworkPtr = optKittyPng ? kittyCoverArtReservation(columns) : Element{};
    }
    else
    {
      artworkPtr = renderCoverArtPreview(optPreview);
    }

    if (artworkPtr == nullptr)
    {
      return text(std::string{i18n::requiredText(textCatalog, i18n::MessageId::CoverArtNone)}) | dim;
    }

    if (optArtworkBox != nullptr)
    {
      artworkPtr = std::move(artworkPtr) | reflect(*optArtworkBox);
    }

    // Artwork keeps its exact cells only when something beside it absorbs the
    // rest of the pane, which also centres it.
    return hbox({filler(), std::move(artworkPtr), filler()});
  }

  bool detailPaneShowsCoverArt(std::int32_t const availableRows)
  {
    auto const worstCaseMetadataRows = static_cast<std::int32_t>(trackDetailFields().size());

    return availableRows >= kCoverArtRows + kDetailChromeRows + worstCaseMetadataRows;
  }

  std::string kittyCoverArtPaintEscape(ftxui::Box const& coverBox, std::vector<std::byte> const& png)
  {
    if (!isValidBox(coverBox))
    {
      return {};
    }

    auto const columns = coverBox.x_max - coverBox.x_min + 1;
    auto const rows = coverBox.y_max - coverBox.y_min + 1;

    return std::format(
      "\033[s\033[{};{}H{}\033[u", coverBox.y_min + 1, coverBox.x_min + 1, kittyImageEscape(png, columns, rows));
  }

  void updateKittyCoverArt(KittyPaintState& state,
                           ResourceId const cachedCoverArtId,
                           ftxui::Box const& coverBox,
                           std::optional<std::vector<std::byte>> const& optKittyCoverArtPng,
                           KittyEscapeSink const& sink)
  {
    auto const shouldShow = optKittyCoverArtPng && isValidBox(coverBox);

    if (shouldShow)
    {
      if (isSameKittyImage(state, cachedCoverArtId, coverBox))
      {
        return;
      }

      // A first paint has nothing to delete, so it is already one operation.
      // A replacement is a delete plus a draw and has to be bracketed, or the
      // terminal may render the moment it holds neither.
      auto escape = std::string{};

      if (state.visible)
      {
        escape.append(kSynchronizedUpdateBegin);
        escape.append(kittyDeleteImageEscape(kKittyCoverArtImageId));
        escape.append(kittyCoverArtPaintEscape(coverBox, *optKittyCoverArtPng));
        escape.append(kSynchronizedUpdateEnd);
      }
      else
      {
        escape = kittyCoverArtPaintEscape(coverBox, *optKittyCoverArtPng);
      }

      sink(escape);
      state.paintedCoverArtId = cachedCoverArtId;
      state.paintedCoverBox = coverBox;
      state.visible = true;
      return;
    }

    if (!shouldShow && state.visible)
    {
      sink(kittyDeleteImageEscape(kKittyCoverArtImageId));
      state.visible = false;
      state.paintedCoverArtId = kInvalidResourceId;
      state.paintedCoverBox = {};
    }
  }

  ftxui::Element centerPopover(ftxui::Element popoverPtr)
  {
    using namespace ftxui;

    return vbox({
      filler(),
      hbox({
        filler(),
        popoverClearHalo(std::move(popoverPtr)),
        filler(),
      }),
      filler(),
    });
  }

  std::int32_t detailPaneColumns(i18n::MessageCatalog const& textCatalog,
                                 std::int32_t const terminalColumns,
                                 std::int32_t const coverColumns)
  {
    auto const labelColumns = detailLabelContentColumns(textCatalog);
    auto contentColumns = std::max(coverColumns, labelColumns + kDetailValueColumns);
    contentColumns =
      std::max(contentColumns, cellWidth(i18n::requiredText(textCatalog, i18n::MessageId::TrackDetailTitle)));
    contentColumns =
      std::max(contentColumns, cellWidth(i18n::requiredText(textCatalog, i18n::MessageId::TrackNoSelection)));

    return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element detailPane(i18n::MessageCatalog const& textCatalog,
                            TrackListEntry const* selectedTrack,
                            ftxui::Element coverElementPtr,
                            std::int32_t const columns)
  {
    using namespace ftxui;

    auto const bodyColumns = style::popupPanelBodyColumns(columns);
    auto const split = detailBodySplit(textCatalog, bodyColumns);
    auto detailElements = Elements{};

    if (selectedTrack == nullptr)
    {
      detailElements.push_back(
        text(ellipsizeToCellWidth(i18n::requiredText(textCatalog, i18n::MessageId::TrackNoSelection), bodyColumns)) |
        dim);
    }
    else
    {
      for (auto const& line : trackDetailLines(textCatalog, selectedTrack->row))
      {
        detailElements.push_back(hbox({
          text(fitCellText(detailLabelText(line.label, split.labelColumns), split.labelColumns)) | dim,
          text(ellipsizeToCellWidth(line.value, split.valueColumns)),
        }));
      }
    }

    auto bodyElements = Elements{};

    // A selection without artwork is the pane's only content, so the separator
    // belongs to the artwork rather than standing on its own above metadata.
    if (selectedTrack != nullptr && coverElementPtr != nullptr)
    {
      bodyElements.push_back(std::move(coverElementPtr));
      bodyElements.push_back(separator());
    }

    bodyElements.push_back(vbox(std::move(detailElements)) | frame | flex);

    return style::popupPanel(
             i18n::requiredText(textCatalog, i18n::MessageId::TrackDetailTitle), vbox(std::move(bodyElements))) |
           size(WIDTH, EQUAL, columns);
  }

  ftxui::Element helpPane(i18n::MessageCatalog const& textCatalog, std::int32_t const terminalColumns)
  {
    using namespace ftxui;

    auto lines = resolveHelpPaneLines(textCatalog);
    auto const title = std::string{overlayLabel(textCatalog, Overlay::Help)};
    auto const columns = resolvedHelpPaneColumns(lines, title, terminalColumns);

    auto rows = Elements{};
    rows.reserve(kHelpPaneLines.size() + kHelpPaneSeparatorCount);

    for (std::size_t line = 0; line < kHelpPaneFooterLine; ++line)
    {
      rows.push_back(text(std::move(lines[line])));
    }

    rows.push_back(separator());
    rows.push_back(text(std::move(lines[kHelpPaneFooterLine])) | dim);

    return style::popupPanel(title, vbox(std::move(rows))) | size(WIDTH, EQUAL, columns);
  }
} // namespace ao::tui
