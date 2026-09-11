// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Render.h"

#include "Command.h"
#include "CoverArt.h"
#include "Keymap.h"
#include "MouseBindings.h"
#include "ShellInteractionModel.h"
#include "ShellText.h"
#include "Style.h"
#include "TextCell.h"
#include "TrackDetailLines.h"
#include "TrackListEntry.h"
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/field/TrackFieldFormatter.h>
#include <ao/uimodel/library/detail/TrackFieldGrid.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui
{
  namespace
  {
    /// Scroll read-only metadata by its first visible row, without centering a focus target.
    class DetailScrollNode final : public ftxui::Node
    {
    public:
      DetailScrollNode(ftxui::Element contentPtr, std::int32_t const scrollRow, ftxui::Box const* const revealBox)
        : Node{{std::move(contentPtr)}}, _scrollRow{scrollRow}, _revealBox{revealBox}
      {
      }

      void ComputeRequirement() override
      {
        Node::ComputeRequirement();
        requirement_ = children_.front()->requirement();
        requirement_.min_y = 0;
        requirement_.flex_grow_y = 1;
        requirement_.flex_shrink_y = 1;
      }

      void SetBox(ftxui::Box const box) override
      {
        Node::SetBox(box);
        auto const visibleRows = std::max(0, box.y_max - box.y_min + 1);
        auto const contentRows = std::max(visibleRows, children_.front()->requirement().min_y);
        auto contentBox = box;
        contentBox.y_min -= std::clamp(_scrollRow, 0, contentRows - visibleRows);
        contentBox.y_max = contentBox.y_min + contentRows - 1;
        children_.front()->SetBox(contentBox);

        if (_revealBox != nullptr && !_revealBox->IsEmpty() && visibleRows > 0)
        {
          auto const delta =
            _revealBox->y_min < box.y_min ? _revealBox->y_min - box.y_min : std::max(0, _revealBox->y_max - box.y_max);
          auto const offset = std::clamp(box.y_min - contentBox.y_min + delta, 0, contentRows - visibleRows);
          contentBox.y_min = box.y_min - offset;
          contentBox.y_max = contentBox.y_min + contentRows - 1;
          children_.front()->SetBox(contentBox);
        }
      }

      void Render(ftxui::Screen& screen) override
      {
        auto const stencil = screen.stencil;
        screen.stencil = ftxui::Box::Intersection(stencil, box_);
        Node::Render(screen);
        screen.stencil = stencil;
      }

    private:
      std::int32_t _scrollRow;
      ftxui::Box const* _revealBox;
    };

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
    constexpr std::int32_t kDetailColumnGap = 2;
    /// Frame edges and the artwork separator: the rows Detail spends on chrome.
    constexpr std::int32_t kDetailChromeRows = 3;

    struct HelpPaneRowSpec final
    {
      i18n::MessageId groupId = i18n::MessageId::Count;
      i18n::MessageId descriptionId = i18n::MessageId::Count;
      std::string_view command{};
    };

    constexpr auto kHelpPaneRowSpecs = std::to_array<HelpPaneRowSpec>({
      {.groupId = i18n::MessageId::TuiKeyGroupNavigation,
       .descriptionId = i18n::MessageId::TuiGoToTitle,
       .command = ":goto"},
      {.descriptionId = i18n::MessageId::TuiShellHelpCurrentTrack, .command = ":current"},
      {.descriptionId = i18n::MessageId::TuiGoToArtist, .command = ":artist"},
      {.descriptionId = i18n::MessageId::TuiGoToAlbum, .command = ":album"},
      {.descriptionId = i18n::MessageId::TuiWorkspaceBack, .command = ":back"},
      {.descriptionId = i18n::MessageId::TuiWorkspaceForward, .command = ":forward"},
      {.groupId = i18n::MessageId::TuiKeyGroupBrowse, .descriptionId = i18n::MessageId::TuiNavigationFocus},
      {.descriptionId = i18n::MessageId::TuiSettingsPreviousRow},
      {.descriptionId = i18n::MessageId::TuiSettingsNextRow},
      {.descriptionId = i18n::MessageId::TuiShellHelpPreviousNextGroup},
      {.descriptionId = i18n::MessageId::TuiShellHelpQuickFilter, .command = ":filter <text>"},
      {.descriptionId = i18n::MessageId::TuiShellHelpClearFilter, .command = ":clear"},
      {.descriptionId = i18n::MessageId::TuiShellHelpReloadList, .command = ":reload"},
      {.descriptionId = i18n::MessageId::TuiShellHelpScan, .command = ":scan / :scan cancel"},
      {.groupId = i18n::MessageId::TuiKeyGroupPlayback,
       .descriptionId = i18n::MessageId::TuiShellHelpPlayback,
       .command = ":play :pause :stop"},
      {.descriptionId = i18n::MessageId::PlaybackControlPreviousTrack, .command = ":previous"},
      {.descriptionId = i18n::MessageId::PlaybackControlNextTrack, .command = ":next"},
      {.descriptionId = i18n::MessageId::PlaybackActionToggleShuffle, .command = ":shuffle"},
      {.descriptionId = i18n::MessageId::PlaybackActionCycleRepeat, .command = ":repeat"},
      {.descriptionId = i18n::MessageId::TuiSettingsSeekBack},
      {.descriptionId = i18n::MessageId::TuiSettingsSeekForward},
      {.descriptionId = i18n::MessageId::TuiSettingsVolumeDown},
      {.descriptionId = i18n::MessageId::TuiSettingsVolumeUp},
      {.groupId = i18n::MessageId::TuiKeyGroupSelection,
       .descriptionId = i18n::MessageId::TuiShellHelpSelect,
       .command = ":select toggle / :select visual / :select all / :select clear"},
      {.descriptionId = i18n::MessageId::TuiShellDetailEditProperties, .command = ":edit"},
      {.descriptionId = i18n::MessageId::TuiShellDetailEditTags, .command = ":tags"},
      {.groupId = i18n::MessageId::TuiKeyGroupPanels,
       .descriptionId = i18n::MessageId::TuiShellHelpChooseList,
       .command = ":lists"},
      {.descriptionId = i18n::MessageId::TuiNavigationPin, .command = ":sidebar"},
      {.descriptionId = i18n::MessageId::TuiShellHelpTrackDetail, .command = ":detail"},
      {.descriptionId = i18n::MessageId::TuiDetailFocus},
      {.descriptionId = i18n::MessageId::TuiPanelResize},
      {.descriptionId = i18n::MessageId::TuiBeginPanelResize},
      {.descriptionId = i18n::MessageId::TuiShellHelpAudioPipeline, .command = ":pipeline"},
      {.descriptionId = i18n::MessageId::TuiShellHelpOutputDevice, .command = ":output"},
      {.descriptionId = i18n::MessageId::TuiShellHelpChooseView, .command = ":views"},
      {.descriptionId = i18n::MessageId::TuiShellHelpNotifications, .command = ":notifications"},
      {.descriptionId = i18n::MessageId::TuiShellHelpSwitchPresentation, .command = ":view <id>"},
      {.groupId = i18n::MessageId::TuiKeyGroupApplication,
       .descriptionId = i18n::MessageId::TuiSettingsTitle,
       .command = ":settings"},
      {.descriptionId = i18n::MessageId::TuiShellHelpQuit, .command = ":quit"},
    });
    constexpr std::int32_t kHelpPaneColumnGap = 2;

    struct ResolvedHelpPaneRow final
    {
      std::string group{};
      std::string shortcut{};
      std::string description{};
    };

    struct ResolvedHelpPane final
    {
      std::array<ResolvedHelpPaneRow, kHelpPaneRowSpecs.size()> rows{};
      std::string footer{};
      std::int32_t shortcutColumns = 0;
      std::int32_t descriptionColumns = 0;
    };

    struct HelpPaneColumnWidths final
    {
      std::int32_t shortcut = 0;
      std::int32_t description = 0;
      std::int32_t gap = 0;
    };

    std::string helpShortcut(KeymapPlan const& keymapPlan, HelpPaneRowSpec const& spec)
    {
      auto joinComplete = [&](std::span<KeyAction const> const actions)
      {
        auto result = std::string{};

        for (auto const action : actions)
        {
          auto const shortcut = keymapPlan.shortcutFor(action);

          if (shortcut.empty())
          {
            return std::string{};
          }

          if (!result.empty())
          {
            result += " / ";
          }

          result += shortcut;
        }

        return result;
      };

      if (auto const optCommand = parseCommand(spec.command); optCommand)
      {
        if (auto shortcut = commandShortcut(keymapPlan, optCommand->action); !shortcut.empty())
        {
          return shortcut;
        }
      }

      switch (spec.descriptionId)
      {
        case i18n::MessageId::TuiSettingsPreviousRow:
          return std::string{keymapPlan.shortcutFor(KeyAction::PreviousRow)};
        case i18n::MessageId::TuiSettingsNextRow: return std::string{keymapPlan.shortcutFor(KeyAction::NextRow)};
        case i18n::MessageId::TuiBeginPanelResize:
          return std::string{keymapPlan.shortcutFor(KeyAction::BeginPanelResize)};
        case i18n::MessageId::TuiPanelResize: return "Shift+← / →";
        case i18n::MessageId::TuiDetailFocus: return std::string{keymapPlan.shortcutFor(KeyAction::FocusDetails)};
        case i18n::MessageId::TuiNavigationFocus:
          return std::string{keymapPlan.shortcutFor(KeyAction::SwitchWorkspaceFocus)};
        case i18n::MessageId::TuiShellHelpQuickFilter:
          return std::string{keymapPlan.shortcutFor(KeyAction::OpenQuickFilter)};
        case i18n::MessageId::TuiShellHelpPlayback:
        {
          constexpr auto kActions =
            std::to_array({KeyAction::PlaySelection, KeyAction::PlaybackPlayPause, KeyAction::PlaybackStop});
          return joinComplete(kActions);
        }
        case i18n::MessageId::TuiShellHelpPreviousNextGroup:
        {
          constexpr auto kActions = std::to_array({KeyAction::PreviousSection, KeyAction::NextSection});
          return joinComplete(kActions);
        }
        case i18n::MessageId::TuiSettingsSeekBack: return std::string{keymapPlan.shortcutFor(KeyAction::SeekBackward)};
        case i18n::MessageId::TuiSettingsSeekForward:
          return std::string{keymapPlan.shortcutFor(KeyAction::SeekForward)};
        case i18n::MessageId::TuiSettingsVolumeDown: return std::string{keymapPlan.shortcutFor(KeyAction::VolumeDown)};
        case i18n::MessageId::TuiSettingsVolumeUp: return std::string{keymapPlan.shortcutFor(KeyAction::VolumeUp)};
        case i18n::MessageId::TuiShellHelpSelect:
        {
          constexpr auto kActions = std::to_array(
            {KeyAction::SelectToggle, KeyAction::SelectVisual, KeyAction::SelectAll, KeyAction::SelectClear});
          return joinComplete(kActions);
        }
        case i18n::MessageId::TuiShellHelpQuit: return std::string{keymapPlan.shortcutFor(KeyAction::Quit)};
        default: return {};
      }
    }

    ResolvedHelpPane resolveHelpPane(i18n::MessageCatalog const& textCatalog, KeymapPlan const& keymapPlan)
    {
      auto result = ResolvedHelpPane{};

      for (std::size_t index = 0; index < kHelpPaneRowSpecs.size(); ++index)
      {
        auto const& spec = kHelpPaneRowSpecs[index];
        auto& row = result.rows[index];

        if (spec.groupId != i18n::MessageId::Count)
        {
          row.group = i18n::requiredText(textCatalog, spec.groupId);
        }

        row.shortcut = helpShortcut(keymapPlan, spec);

        if (row.shortcut.empty())
        {
          row.shortcut = spec.command;
        }

        row.description = i18n::requiredText(textCatalog, spec.descriptionId);
        result.shortcutColumns = std::max(result.shortcutColumns, cellWidth(row.shortcut));
        result.descriptionColumns = std::max(result.descriptionColumns, cellWidth(row.description));
      }

      result.footer = chromeText(textCatalog, i18n::MessageId::TuiShellHelpFooter);
      return result;
    }

    std::int32_t resolvedHelpPaneColumns(ResolvedHelpPane const& help,
                                         std::string_view const title,
                                         std::int32_t const terminalColumns)
    {
      auto const rowColumns = help.shortcutColumns + help.descriptionColumns + kHelpPaneColumnGap;
      auto const contentColumns = std::max({cellWidth(title), cellWidth(help.footer), rowColumns});

      return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
    }

    HelpPaneColumnWidths helpPaneColumnWidths(ResolvedHelpPane const& help, std::int32_t const panelColumns)
    {
      auto const bodyColumns = style::popupPanelBodyColumns(panelColumns);
      auto const gap = bodyColumns > kHelpPaneColumnGap ? kHelpPaneColumnGap : 0;
      auto const availableColumns = bodyColumns - gap;
      auto const shortcutColumns = std::min(help.shortcutColumns, availableColumns / 2);
      return HelpPaneColumnWidths{
        .shortcut = shortcutColumns, .description = availableColumns - shortcutColumns, .gap = gap};
    }

    ftxui::Element fixedHelpText(std::string_view const value, std::int32_t const columns)
    {
      return ftxui::text(fitCellText(ellipsizeToCellWidth(value, columns), columns)) |
             ftxui::size(ftxui::WIDTH, ftxui::EQUAL, columns);
    }

    ftxui::Element helpPaneRow(ResolvedHelpPaneRow const& row, HelpPaneColumnWidths const widths)
    {
      auto cells = ftxui::Elements{};

      if (widths.shortcut > 0)
      {
        cells.push_back(fixedHelpText(row.shortcut, widths.shortcut));
        cells.push_back(ftxui::text(std::string(static_cast<std::size_t>(widths.gap), ' ')));
      }

      cells.push_back(fixedHelpText(row.description, widths.description) | ftxui::flex);
      return ftxui::hbox(std::move(cells));
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

    /// The label column this locale asks for, capped and including the column gap.
    std::int32_t detailLabelContentColumns(i18n::MessageCatalog const& textCatalog)
    {
      std::int32_t labelColumns = 0;

      for (auto const field : trackDetailFields())
      {
        labelColumns = std::max(labelColumns, cellWidth(uimodel::trackFieldLabel(textCatalog, field)));
      }

      return std::min(labelColumns, kDetailLabelColumns) + kDetailColumnGap;
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
      return ellipsizeToCellWidth(label, std::max(0, labelColumns - kDetailColumnGap));
    }

    ftxui::Element wrappedDetailText(std::string_view const value, std::int32_t const columns)
    {
      using namespace ftxui;
      auto rows = Elements{};

      for (auto& line : wrapCellText(value, columns))
      {
        rows.push_back(text(std::move(line)));
      }

      return vbox(std::move(rows));
    }

    ftxui::Element detailField(TrackDetailLine const& line, DetailBodySplit const split)
    {
      using namespace ftxui;
      using Kind = TrackDetailLine::Kind;
      auto valuePtr = wrappedDetailText(line.value, split.valueColumns);

      if (line.kind == Kind::Title)
      {
        valuePtr = std::move(valuePtr) | bold | style::accent();
      }

      return hbox({text(fitCellText(detailLabelText(line.label, split.labelColumns), split.labelColumns)) | dim,
                   std::move(valuePtr) | xflex});
    }

    ftxui::Element detailMetadata(i18n::MessageCatalog const& textCatalog,
                                  rt::TrackRow const& row,
                                  std::int32_t const bodyColumns,
                                  DetailPaneOptions const& options)
    {
      using namespace ftxui;
      auto const split = detailBodySplit(textCatalog, bodyColumns);
      auto elements = Elements{};
      auto const lines = trackDetailLines(textCatalog, row);
      auto const technicalSummary =
        uimodel::formatTechnicalSummary(row.codec, row.sampleRate, row.bitDepth, row.bitrate);

      auto header = [&](std::size_t const index, std::string label)
      {
        auto headerPtr = text(fitCellText(
          ellipsizeToCellWidth(std::string{options.sections.expanded[index] ? "▾ " : "▸ "} + label, bodyColumns),
          bodyColumns));
        headerPtr = std::move(headerPtr) |
                    (options.focused && options.sections.selected == index ? style::selected() : style::accent());

        if (options.headerBoxes != nullptr)
        {
          headerPtr = std::move(headerPtr) | reflectLayout((*options.headerBoxes)[index]);
        }

        elements.push_back(std::move(headerPtr));
      };
      auto const metadataSummary = uimodel::formatMetadataHeader(trackDisplayTitle(textCatalog, row), row.artist);

      header(0,
             options.sections.expanded[0]
               ? std::string{i18n::requiredText(textCatalog, i18n::MessageId::TrackMetadataHeading)}
               : metadataSummary);

      if (options.sections.expanded[0])
      {
        for (auto const& line : lines)
        {
          elements.push_back(detailField(line, split));
        }
      }

      elements.push_back(text(""));
      header(1,
             options.sections.expanded[1] || technicalSummary.empty()
               ? std::string{i18n::requiredText(textCatalog, i18n::MessageId::TrackAudioPropertiesHeading)}
               : technicalSummary);

      if (options.sections.expanded[1])
      {
        for (auto const& line : trackDetailTechnicalLines(textCatalog, row))
        {
          elements.push_back(detailField(line, split));
        }
      }

      return vbox(std::move(elements));
    }
  } // namespace

  void defaultKittyEscapeSink(std::string_view const escapeSequence)
  {
    std::print("{}", escapeSequence);
    std::fflush(stdout);
  }

  bool isValidBox(ftxui::Box const& box)
  {
    return box.x_min >= 0 && box.y_min >= 0 && box.x_max > box.x_min && box.y_max > box.y_min;
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

  bool isDetailPaneShowingCoverArt(std::int32_t const availableRows)
  {
    // Reserve a useful first page of metadata regardless of the selected track's length.
    constexpr std::int32_t kMinimumMetadataRows = 10;
    return availableRows >= kCoverArtRows + kDetailChromeRows + kMinimumMetadataRows;
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
        style::popoverClearHalo(std::move(popoverPtr)),
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
      std::max(contentColumns, cellWidth(i18n::requiredText(textCatalog, i18n::MessageId::TrackNoSelection)));

    return style::popupPanelColumnsForContent(contentColumns, terminalColumns);
  }

  ftxui::Element dockDetailPane(ftxui::Element workspacePtr,
                                ftxui::Element detailPtr,
                                std::int32_t const columns,
                                ftxui::Box* const toggleBox,
                                bool const hovered,
                                style::PanelDividerOptions const options)
  {
    using namespace ftxui;
    auto const workspaceOffset = std::max(0, columns - (options.separateBorders ? 0 : 1));
    auto offset = [](std::int32_t width) { return filler() | size(WIDTH, EQUAL, width); };

    return dbox(
      {hbox({std::move(workspacePtr) | flex, offset(workspaceOffset)}),
       hbox({filler(), std::move(detailPtr)}),
       hbox({filler(), style::panelDivider("›", toggleBox, hovered, options), offset(std::max(0, columns - 1))})});
  }

  ftxui::Element collapsedDetailPane(ftxui::Element workspacePtr,
                                     ftxui::Box& toggleBox,
                                     bool const hovered,
                                     bool const revealOnHover,
                                     ftxui::Box* const hoverBox)
  {
    using namespace ftxui;
    auto edgePtr = vbox({filler(), style::panelIndicator("‹", toggleBox, hovered, revealOnHover), filler()});

    if (hoverBox != nullptr)
    {
      edgePtr = std::move(edgePtr) | reflect(*hoverBox);
    }

    return dbox({std::move(workspacePtr), hbox({filler(), std::move(edgePtr)})});
  }

  ftxui::Element detailPane(i18n::MessageCatalog const& textCatalog,
                            TrackListEntry const* selectedTrack,
                            ftxui::Element coverElementPtr,
                            std::int32_t const columns,
                            PanelMouseRegions* const regions,
                            std::int32_t const scrollRow,
                            DetailPaneOptions const options)
  {
    using namespace ftxui;

    if (options.headerBoxes != nullptr)
    {
      options.headerBoxes->fill(kEmptyMouseBox);
    }

    auto const bodyColumns = style::popupPanelBodyColumns(columns);
    auto metadataPtr =
      selectedTrack == nullptr
        ? wrappedDetailText(i18n::requiredText(textCatalog, i18n::MessageId::TrackNoSelection), bodyColumns) | dim
        : detailMetadata(textCatalog, selectedTrack->row, bodyColumns, options);

    if (regions != nullptr)
    {
      *regions = {};
      metadataPtr = std::move(metadataPtr) | reflectLayout(regions->contentBox);
    }

    metadataPtr = std::make_shared<DetailScrollNode>(std::move(metadataPtr),
                                                     scrollRow,
                                                     options.headerBoxes != nullptr && options.focused &&
                                                         options.sections.revealSelected &&
                                                         options.sections.selected < options.headerBoxes->size()
                                                       ? &(*options.headerBoxes)[options.sections.selected]
                                                       : nullptr) |
                  flex;

    if (regions != nullptr)
    {
      metadataPtr = std::move(metadataPtr) | reflect(regions->navigationBox);
    }

    auto bodyElements = Elements{};

    if (selectedTrack != nullptr && coverElementPtr != nullptr)
    {
      bodyElements.push_back(std::move(coverElementPtr));
      bodyElements.push_back(text(""));
    }

    bodyElements.push_back(std::move(metadataPtr));
    auto bodyPtr = vbox(std::move(bodyElements));

    auto panelPtr = style::popupPanel("", std::move(bodyPtr)) | size(WIDTH, EQUAL, columns);
    return regions == nullptr ? panelPtr : std::move(panelPtr) | reflect(regions->box);
  }

  ftxui::Element helpPane(i18n::MessageCatalog const& textCatalog,
                          KeymapPlan const& keymapPlan,
                          std::int32_t const terminalColumns,
                          PanelMouseRegions* const mouseRegions,
                          std::int32_t const scrollRow)
  {
    using namespace ftxui;

    auto help = resolveHelpPane(textCatalog, keymapPlan);
    auto const title = std::string{overlayLabel(textCatalog, Overlay::Help)};
    auto const columns = resolvedHelpPaneColumns(help, title, terminalColumns);
    auto const widths = helpPaneColumnWidths(help, columns);

    auto rows = Elements{};
    rows.reserve(kHelpPaneRowSpecs.size());

    for (auto const& row : help.rows)
    {
      if (!row.group.empty())
      {
        if (!rows.empty())
        {
          rows.push_back(separator());
        }

        rows.push_back(text(ellipsizeToCellWidth(row.group, style::popupPanelBodyColumns(columns))) | bold |
                       style::accent());
      }

      rows.push_back(helpPaneRow(row, widths));
    }

    auto bodyPtr = vbox(std::move(rows));

    if (mouseRegions != nullptr)
    {
      bodyPtr = std::move(bodyPtr) | reflectLayout(mouseRegions->contentBox) | focusPosition(0, scrollRow) |
                vscroll_indicator | yframe | ftxui::reflect(mouseRegions->navigationBox) | flex;
    }

    auto panelPtr =
      style::titledPanel(
        title,
        vbox(
          {(mouseRegions != nullptr ? style::scrollablePanelBody : style::panelBody)(std::move(bodyPtr)),
           style::panelBody(separator()),
           style::panelBody(text(ellipsizeToCellWidth(help.footer, style::popupPanelBodyColumns(columns))) | dim)})) |
      size(WIDTH, EQUAL, columns);
    return mouseRegions != nullptr ? mousePanel(std::move(panelPtr), *mouseRegions) : std::move(panelPtr);
  }
} // namespace ao::tui
