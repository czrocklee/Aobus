// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/Render.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/TuiRenderTestSupport.h"
#include "tui/CommandPalettePanel.h"
#include "tui/CoverArt.h"
#include "tui/NotificationCenterPanel.h"
#include "tui/PresentationPanel.h"
#include "tui/ShellInteractionModel.h"
#include "tui/StatusBar.h"
#include "tui/Style.h"
#include "tui/TextCell.h"
#include "tui/TrackDetailLines.h"
#include "tui/TrackListEntry.h"
#include "tui/TrackPresentationNavigation.h"
#include "tui/TuiHitRegions.h"
#include "tui/TuiText.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/library/presentation/TrackPresentationText.h>
#include <ao/uimodel/status/activity/ActivityStatusViewState.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    TrackListEntry englishTrackListEntry(rt::TrackRow const& row)
    {
      return makeTrackListEntry(ao::test::englishMessageCatalog(), row);
    }

    std::int32_t englishDetailPaneColumns(std::int32_t const terminalColumns)
    {
      return detailPaneColumns(ao::test::englishMessageCatalog(), terminalColumns, kCoverArtDefaultColumns);
    }

    ftxui::Element englishDetailPane(TrackListEntry const* const selectedTrack,
                                     ftxui::Element coverElementPtr,
                                     std::int32_t const columns)
    {
      return detailPane(ao::test::englishMessageCatalog(), selectedTrack, std::move(coverElementPtr), columns);
    }

    /// The upper-half block a Blocks-mode cover paints its cells with.
    constexpr std::string_view kBlockArtworkGlyph = "▀";
    constexpr std::int32_t kDetailLabelPercent = 40;
    constexpr std::int32_t kMinimumDetailValueColumns = 12;
    /// The cap the pane puts on a field label, matching the GTK detail grid.
    constexpr std::int32_t kDetailLabelColumns = 12;
    constexpr std::int32_t kDetailValueColumns = 24;

    constexpr std::string_view kSynchronizedUpdateBegin = "\033[?2026h";
    constexpr std::string_view kSynchronizedUpdateEnd = "\033[?2026l";
    constexpr std::string_view kKittyDeletePrefix = "\033_Ga=d";
    constexpr std::string_view kKittyDrawPrefix = "\033[s";

    /// Delete then draw, both inside one synchronized update, in one write.
    void checkBracketedReplacement(std::string const& escape)
    {
      CHECK(escape.starts_with(kSynchronizedUpdateBegin));
      CHECK(escape.ends_with(kSynchronizedUpdateEnd));

      auto const deleteOffset = escape.find(kKittyDeletePrefix);
      auto const drawOffset = escape.find(kKittyDrawPrefix);

      REQUIRE(deleteOffset != std::string::npos);
      REQUIRE(drawOffset != std::string::npos);
      CHECK(deleteOffset < drawOffset);
    }

    std::int32_t boxColumns(ftxui::Box const& box)
    {
      return box.x_max - box.x_min + 1;
    }

    std::int32_t boxRows(ftxui::Box const& box)
    {
      return box.y_max - box.y_min + 1;
    }

    /**
     * @brief Renders @p panePtr where the shell puts it: beside a flexible
     *        workspace, which is what makes its declared width its real one.
     */
    RenderedElement renderBesideWorkspace(ftxui::Element panePtr,
                                          ftxui::Box& paneBox,
                                          std::int32_t const terminalColumns = 120,
                                          std::int32_t const terminalRows = 40)
    {
      return renderElement(ftxui::hbox({ftxui::filler() | ftxui::flex, std::move(panePtr) | ftxui::reflect(paneBox)}),
                           terminalColumns,
                           terminalRows);
    }

    std::int32_t widestDetailLabelColumns(i18n::MessageCatalog const& textCatalog)
    {
      std::int32_t widest = 0;

      for (auto const field : trackDetailFields())
      {
        widest = std::max(widest, cellWidth(uimodel::trackFieldLabel(textCatalog, field)));
      }

      return std::min(widest, kDetailLabelColumns) + cellWidth(": ");
    }

    rt::TrackRow sparseRow()
    {
      return rt::TrackRow{.id = TrackId{1}, .title = "Untagged"};
    }

    rt::TrackRow fullyPopulatedRow()
    {
      return rt::TrackRow{.id = TrackId{2},
                          .title = "A very long title that would widen any pane sized from its own selection",
                          .artist = "An equally long artist credit that keeps going well past the pane",
                          .album = "Album",
                          .albumArtist = "Album Artist",
                          .genre = "Genre",
                          .composer = "Composer",
                          .conductor = "Conductor",
                          .ensemble = "Ensemble",
                          .soloist = "Soloist",
                          .tags = "one, two, three",
                          .duration = std::chrono::seconds{299},
                          .year = 2014,
                          .trackNumber = 7,
                          .trackTotal = 12,
                          .sampleRate = 44100,
                          .bitDepth = 16,
                          .codec = AudioCodec::Flac};
    }

    CoverArtRows solidPreview(std::int32_t const columns = kCoverArtDefaultColumns)
    {
      return CoverArtRows(static_cast<std::size_t>(kCoverArtRows),
                          std::vector<CoverArtCell>(static_cast<std::size_t>(columns), CoverArtCell{}));
    }

    ftxui::Element commandPalettePanel(ShellInteractionModel const& shell, std::int32_t const columns = 0)
    {
      return ao::tui::commandPalettePanel(ao::test::englishMessageCatalog(), shell, columns);
    }

    ftxui::Element quickFilterCompletionPanel(ShellInteractionModel const& shell,
                                              std::int32_t const columns = 0,
                                              std::string_view const filterError = {})
    {
      return ao::tui::quickFilterCompletionPanel(ao::test::englishMessageCatalog(), shell, columns, filterError);
    }

    ftxui::Element helpPane(std::int32_t const columns = 0)
    {
      return ao::tui::helpPane(ao::test::englishMessageCatalog(), columns);
    }

    ftxui::Element statusBar(StatusBarViewState const& state)
    {
      return ao::tui::statusBar(ao::test::englishMessageCatalog(), state);
    }

    ftxui::Element notificationCenterPanel(uimodel::ActivityStatusViewState const& state,
                                           std::vector<NotificationDetailRowHitRegion>* const rowHitRegions = nullptr,
                                           std::int32_t const columns = 0)
    {
      return ao::tui::notificationCenterPanel(ao::test::englishMessageCatalog(), state, rowHitRegions, columns);
    }

    ftxui::Element presentationPanel(std::vector<TrackPresentationNavEntry> const& items,
                                     std::string_view const activePresentationId,
                                     std::int32_t const selectedIndex,
                                     std::vector<PresentationRowHitRegion>* const rowHitRegions = nullptr,
                                     std::int32_t const columns = 0)
    {
      return ao::tui::presentationPanel(
        ao::test::englishMessageCatalog(), items, activePresentationId, selectedIndex, rowHitRegions, columns);
    }

    std::int32_t presentationPanelColumns(std::vector<TrackPresentationNavEntry> const& items,
                                          std::string_view const activePresentationId,
                                          std::int32_t const terminalColumns)
    {
      return ao::tui::presentationPanelColumns(
        ao::test::englishMessageCatalog(), items, activePresentationId, terminalColumns);
    }
  } // namespace

  TEST_CASE("i18n::MessageCatalog - resolves German and pseudo shell copy", "[tui][unit][localization]")
  {
    auto const german = ao::test::messageCatalog("de-AT");
    CHECK(tuiChromeText(german, i18n::MessageId::TuiShellCommandPaletteTitle) == "Befehlspalette");
    CHECK(tuiChromeText(german, i18n::MessageId::TuiShellQuickFilterTitle) == "Schnellfilter");
    CHECK(tuiChromeText(german, i18n::MessageId::TuiShellQuickFilterFooter).contains("Enter übernehmen"));
    CHECK(tuiChromeText(german, i18n::MessageId::TuiShellOverlayViews) == "Ansichten");
    CHECK(tuiChromeText(german, i18n::MessageId::TuiShellHintLists).contains("Enter öffnen"));
    CHECK(tuiChromeText(german, i18n::MessageId::TuiLibraryNoSections) == "Keine Abschnitte in dieser Ansicht");
    CHECK(libraryReloadedTracks(german, 2) == "2 Titel neu geladen");
    CHECK(libraryQuickFilterMatched(german, 1) == "Schnellfilter fand 1 Titel");

    auto const pseudo = ao::test::messageCatalog("qps-ploc");
    CHECK(tuiChromeText(pseudo, i18n::MessageId::TuiShellCommandPaletteTitle) != "Command Palette");
    CHECK(tuiChromeText(pseudo, i18n::MessageId::TuiShellQuickFilterTitle) != "Quick Filter");
    CHECK(tuiChromeText(pseudo, i18n::MessageId::TuiShellHintViews).contains("Enter"));
    CHECK(tuiChromeText(pseudo, i18n::MessageId::TuiLibraryNoTracksFound) !=
          "No tracks found. Run `aobus init` in this library first.");
    CHECK(libraryOpenedList(pseudo, "Road Trip").contains("Road Trip"));

    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "view albums");
    auto const narrow = renderElement(commandPalettePanel(pseudo, shell, 32), 32, 8);
    CHECK(narrow.text.contains(":view albums"));
    CHECK_FALSE(narrow.text.empty());
  }

  TEST_CASE("Render - help pane advertises workspace commands", "[tui][unit][render]")
  {
    auto const text = renderText(helpPane());

    CHECK(text.contains(":current"));
    CHECK(text.contains(":view <id>"));
    CHECK(text.contains(":output"));
    CHECK(text.contains(":views"));
    CHECK(text.contains(":notifications"));
    CHECK(text.contains("{ / }"));
  }

  TEST_CASE("Render - side panes size to content and terminal bounds", "[tui][unit][render]")
  {
    auto wideHelpBox = ftxui::Box{};
    auto narrowHelpBox = ftxui::Box{};
    std::ignore = renderBesideWorkspace(helpPane(120), wideHelpBox, 120);
    std::ignore = renderBesideWorkspace(helpPane(30), narrowHelpBox, 30);

    CHECK(boxColumns(wideHelpBox) == helpPaneColumns(ao::test::englishMessageCatalog(), 120));
    CHECK(boxColumns(narrowHelpBox) == helpPaneColumns(ao::test::englishMessageCatalog(), 30));
    CHECK(englishDetailPaneColumns(120) > 0);
    CHECK(englishDetailPaneColumns(40) == 40);
  }

  TEST_CASE("Render - detail pane width follows the locale, never the selection", "[tui][unit][render][detail]")
  {
    auto const columns = englishDetailPaneColumns(120);
    auto const sparse = englishTrackListEntry(sparseRow());
    auto const populated = englishTrackListEntry(fullyPopulatedRow());
    auto emptyBox = ftxui::Box{};
    auto sparseBox = ftxui::Box{};
    auto populatedBox = ftxui::Box{};

    renderBesideWorkspace(englishDetailPane(nullptr, {}, columns), emptyBox);
    renderBesideWorkspace(englishDetailPane(&sparse, {}, columns), sparseBox);
    renderBesideWorkspace(englishDetailPane(&populated, {}, columns), populatedBox);

    CHECK(boxColumns(emptyBox) == columns);
    CHECK(boxColumns(sparseBox) == columns);
    CHECK(boxColumns(populatedBox) == columns);
  }

  TEST_CASE("Render - detail rows keep labels and values inside their budgets", "[tui][unit][render][detail]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto const columns = englishDetailPaneColumns(120);
    auto const bodyColumns = style::popupPanelBodyColumns(columns);
    auto const labelColumns = widestDetailLabelColumns(textCatalog);
    auto const populated = englishTrackListEntry(fullyPopulatedRow());
    auto paneBox = ftxui::Box{};
    auto const rendered = renderBesideWorkspace(englishDetailPane(&populated, {}, columns), paneBox);

    // English labels fit the cap, so they are spelled out in full.
    CHECK(
      rendered.text.contains(std::string{uimodel::trackFieldLabel(textCatalog, rt::TrackField::AlbumArtist)} + ": "));
    CHECK(labelColumns <= kDetailLabelColumns + cellWidth(": "));
    CHECK(labelColumns * 100 <= bodyColumns * kDetailLabelPercent);
    CHECK(bodyColumns - labelColumns >= kMinimumDetailValueColumns);
    // A value too long for its column says where it stopped.
    CHECK(rendered.text.contains("…"));
  }

  TEST_CASE("Render - narrow detail keeps a readable value column", "[tui][unit][render][detail]")
  {
    // The 80x24 floor: the side pane takes half of it and no more.
    constexpr std::int32_t kMinimumTerminalColumns = 80;
    auto const columns = englishDetailPaneColumns(kMinimumTerminalColumns / 2);
    auto const bodyColumns = style::popupPanelBodyColumns(columns);
    auto const populated = englishTrackListEntry(fullyPopulatedRow());
    auto paneBox = ftxui::Box{};
    auto const rendered =
      renderBesideWorkspace(englishDetailPane(&populated, {}, columns), paneBox, kMinimumTerminalColumns, 24);

    CHECK(columns == kMinimumTerminalColumns / 2);
    CHECK(boxColumns(paneBox) == columns);
    CHECK(bodyColumns - (bodyColumns * kDetailLabelPercent / 100) >= kMinimumDetailValueColumns);
    CHECK(rendered.text.contains("…"));
  }

  TEST_CASE("Render - detail omits optional rows a track does not carry", "[tui][unit][render][detail]")
  {
    auto const columns = englishDetailPaneColumns(120);
    auto const sparse = englishTrackListEntry(sparseRow());
    auto paneBox = ftxui::Box{};
    auto const rendered = renderBesideWorkspace(englishDetailPane(&sparse, {}, columns), paneBox);

    CHECK(rendered.text.contains("Title"));
    CHECK(rendered.text.contains("Duration"));
    CHECK_FALSE(rendered.text.contains("Composer"));
    CHECK_FALSE(rendered.text.contains("Sample Rate"));
  }

  TEST_CASE("Render - detail artwork carries no frame of its own", "[tui][unit][render][cover-art]")
  {
    auto const columns = englishDetailPaneColumns(120);
    auto const populated = englishTrackListEntry(fullyPopulatedRow());
    auto artworkBox = ftxui::Box{};
    auto paneBox = ftxui::Box{};
    auto const optPreview = std::optional{solidPreview()};
    auto coverPtr = detailCoverArt(ao::test::englishMessageCatalog(),
                                   CoverArtDeliveryMode::Blocks,
                                   optPreview,
                                   std::nullopt,
                                   kCoverArtDefaultColumns,
                                   &artworkBox);

    REQUIRE(coverPtr != nullptr);

    auto const rendered = renderBesideWorkspace(englishDetailPane(&populated, std::move(coverPtr), columns), paneBox);

    CHECK(rendered.text.contains("Track Detail"));
    CHECK_FALSE(rendered.text.contains("Cover Art"));
    CHECK(boxColumns(artworkBox) == kCoverArtDefaultColumns);
    CHECK(boxRows(artworkBox) == kCoverArtRows);
  }

  TEST_CASE("Render - block and Kitty artwork claim the same cells", "[tui][unit][render][cover-art]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto const populated = englishTrackListEntry(fullyPopulatedRow());
    auto const optPng = std::optional{std::vector{std::byte{0x89}}};

    // The width is whatever the terminal measured, so parity has to hold across
    // the range coverArtColumns can produce, not just at the fallback.
    for (auto const coverColumns : {kMinimumCoverArtColumns, kCoverArtDefaultColumns, kMaximumCoverArtColumns})
    {
      CAPTURE(coverColumns);

      auto const columns = detailPaneColumns(textCatalog, 120, coverColumns);
      auto const optPreview = std::optional{solidPreview(coverColumns)};
      auto blockBox = ftxui::Box{};
      auto kittyBox = ftxui::Box{};
      auto paneBox = ftxui::Box{};

      renderBesideWorkspace(
        englishDetailPane(
          &populated,
          detailCoverArt(textCatalog, CoverArtDeliveryMode::Blocks, optPreview, std::nullopt, coverColumns, &blockBox),
          columns),
        paneBox);
      renderBesideWorkspace(
        englishDetailPane(
          &populated,
          detailCoverArt(textCatalog, CoverArtDeliveryMode::Kitty, std::nullopt, optPng, coverColumns, &kittyBox),
          columns),
        paneBox);

      CHECK(boxColumns(blockBox) == coverColumns);
      CHECK(blockBox.x_min == kittyBox.x_min);
      CHECK(blockBox.x_max == kittyBox.x_max);
      CHECK(blockBox.y_min == kittyBox.y_min);
      CHECK(blockBox.y_max == kittyBox.y_max);
    }
  }

  TEST_CASE("Render - artwork the loader has not published costs one line", "[tui][unit][render][cover-art]")
  {
    auto const& textCatalog = ao::test::englishMessageCatalog();
    auto const columns = englishDetailPaneColumns(120);
    auto const populated = englishTrackListEntry(fullyPopulatedRow());
    auto artworkBox = ftxui::Box{.x_min = 1, .x_max = 24, .y_min = 1, .y_max = 12};
    auto paneBox = ftxui::Box{};
    auto coverPtr = detailCoverArt(
      textCatalog, CoverArtDeliveryMode::Kitty, std::nullopt, std::nullopt, kCoverArtDefaultColumns, &artworkBox);

    REQUIRE(coverPtr != nullptr);

    auto const rendered = renderBesideWorkspace(englishDetailPane(&populated, std::move(coverPtr), columns), paneBox);

    CHECK(rendered.text.contains("No cover art"));
    CHECK_FALSE(rendered.text.contains(kBlockArtworkGlyph));
    // Nothing reserved the cells, so out-of-band paint state sees an empty box.
    CHECK(artworkBox.x_max <= artworkBox.x_min);
  }

  TEST_CASE("Render - artwork waits for a terminal that fits every field", "[tui][unit][render][cover-art]")
  {
    auto const worstCaseRows = static_cast<std::int32_t>(trackDetailFields().size());

    CHECK_FALSE(detailPaneShowsCoverArt(kCoverArtRows + worstCaseRows));
    CHECK(detailPaneShowsCoverArt(kCoverArtRows + worstCaseRows + 3));
    // An 80x24 terminal spends its rows on metadata.
    CHECK_FALSE(detailPaneShowsCoverArt(24 - 2));
  }

  TEST_CASE("Render - Kitty paint state prevents redundant repaint and updates on change",
            "[tui][unit][render][cover-art]")
  {
    auto state = KittyPaintState{};
    auto const resourceId1 = ResourceId{101};
    auto const resourceId2 = ResourceId{202};
    auto const box1 = ftxui::Box{.x_min = 10, .x_max = 34, .y_min = 5, .y_max = 17};
    auto const box2 = ftxui::Box{.x_min = 12, .x_max = 36, .y_min = 6, .y_max = 18};
    auto const invalidBox = ftxui::Box{.x_min = 0, .x_max = 0, .y_min = 0, .y_max = 0};
    auto const optPngBytes =
      std::optional{std::vector{std::byte{0x89}, std::byte{0x50}, std::byte{0x4E}, std::byte{0x47}}};

    auto emitted = std::vector<std::string>{};
    auto const sink = [&emitted](std::string_view const s) { emitted.emplace_back(s); };

    CHECK(isValidBox(box1));
    CHECK_FALSE(isValidBox(invalidBox));
    CHECK(isSameBox(box1, box1));
    CHECK_FALSE(isSameBox(box1, box2));

    // Initially unpainted
    CHECK_FALSE(state.visible);
    CHECK(state.paintedCoverArtId == kInvalidResourceId);

    // A first paint is one operation, so it needs no synchronized update
    updateKittyCoverArt(state, resourceId1, box1, optPngBytes, sink);
    CHECK(state.visible);
    CHECK(state.paintedCoverArtId == resourceId1);
    CHECK(isSameBox(state.paintedCoverBox, box1));
    CHECK(isSameKittyImage(state, resourceId1, box1));
    REQUIRE(emitted.size() == 1);
    CHECK(emitted.front().starts_with("\033[s"));
    CHECK_FALSE(emitted.front().contains(kSynchronizedUpdateBegin));

    // Redundant update with identical resource ID and box is a no-op (preserves state, emits nothing)
    emitted.clear();
    updateKittyCoverArt(state, resourceId1, box1, optPngBytes, sink);
    CHECK(state.visible);
    CHECK(state.paintedCoverArtId == resourceId1);
    CHECK(isSameBox(state.paintedCoverBox, box1));
    CHECK(emitted.empty());

    // Changing the box replaces the image: one write, delete before draw, and
    // bracketed so a terminal honoring mode 2026 cannot render between them.
    emitted.clear();
    updateKittyCoverArt(state, resourceId1, box2, optPngBytes, sink);
    CHECK(state.visible);
    CHECK(state.paintedCoverArtId == resourceId1);
    CHECK(isSameBox(state.paintedCoverBox, box2));
    REQUIRE(emitted.size() == 1);
    checkBracketedReplacement(emitted.front());

    // Changing the resource id replaces the image the same way
    emitted.clear();
    updateKittyCoverArt(state, resourceId2, box2, optPngBytes, sink);
    CHECK(state.visible);
    CHECK(state.paintedCoverArtId == resourceId2);
    CHECK(isSameBox(state.paintedCoverBox, box2));
    REQUIRE(emitted.size() == 1);
    checkBracketedReplacement(emitted.front());

    // Removing the PNG clears state; a lone delete is already one operation
    emitted.clear();
    updateKittyCoverArt(state, resourceId2, box2, std::nullopt, sink);
    CHECK_FALSE(state.visible);
    CHECK(state.paintedCoverArtId == kInvalidResourceId);
    CHECK_FALSE(isValidBox(state.paintedCoverBox));
    REQUIRE(emitted.size() == 1);
    CHECK(emitted.front().starts_with("\033_Ga=d"));
    CHECK_FALSE(emitted.front().contains(kSynchronizedUpdateBegin));

    // Invalid box when PNG is present also clears state and emits delete
    emitted.clear();
    updateKittyCoverArt(state, resourceId1, box1, optPngBytes, sink);
    CHECK(state.visible);
    emitted.clear();
    updateKittyCoverArt(state, resourceId1, invalidBox, optPngBytes, sink);
    CHECK_FALSE(state.visible);
    CHECK(state.paintedCoverArtId == kInvalidResourceId);
    REQUIRE(emitted.size() == 1);
    CHECK(emitted.front().starts_with("\033_Ga=d"));
  }

  TEST_CASE("Render - detail without a selection says so and shows nothing else", "[tui][unit][render][detail]")
  {
    auto const columns = englishDetailPaneColumns(120);
    auto paneBox = ftxui::Box{};
    auto const rendered = renderBesideWorkspace(englishDetailPane(nullptr,
                                                                  detailCoverArt(ao::test::englishMessageCatalog(),
                                                                                 CoverArtDeliveryMode::Blocks,
                                                                                 std::optional{solidPreview()},
                                                                                 std::nullopt,
                                                                                 kCoverArtDefaultColumns),
                                                                  columns),
                                                paneBox);

    CHECK(rendered.text.contains("No track selected"));
    CHECK_FALSE(rendered.text.contains("Title"));
    CHECK_FALSE(rendered.text.contains(kBlockArtworkGlyph));
  }

  TEST_CASE("Render - detail pane width holds in every supported locale", "[tui][unit][render][localization]")
  {
    constexpr std::int32_t kTerminalColumns = 120;

    for (auto const* const locale : {"de-DE", "es-ES", "fr-FR", "ja-JP", "zh-Hans-CN", "zh-Hant-TW", "qps-ploc"})
    {
      auto const textCatalog = ao::test::messageCatalog(locale);
      auto const columns = detailPaneColumns(textCatalog, kTerminalColumns / 2, kCoverArtDefaultColumns);
      auto const sparse = makeTrackListEntry(textCatalog, sparseRow());
      auto const populated = makeTrackListEntry(textCatalog, fullyPopulatedRow());
      auto sparseBox = ftxui::Box{};
      auto populatedBox = ftxui::Box{};

      renderBesideWorkspace(detailPane(textCatalog, &sparse, {}, columns), sparseBox, kTerminalColumns, 40);
      renderBesideWorkspace(detailPane(textCatalog, &populated, {}, columns), populatedBox, kTerminalColumns, 40);

      // A locale whose field names are long shortens them rather than spending
      // the workspace's width on naming fields.
      auto const cappedColumns = style::popupPanelColumnsForContent(
        kDetailLabelColumns + cellWidth(": ") + kDetailValueColumns, kTerminalColumns / 2);

      INFO("locale " << locale);
      CHECK(boxColumns(sparseBox) == columns);
      CHECK(boxColumns(populatedBox) == columns);
      CHECK(columns <= cappedColumns);
      CHECK(columns <= kTerminalColumns / 2);
    }
  }

  TEST_CASE("Render - center popover places content in the screen middle", "[tui][unit][render]")
  {
    auto const rendered =
      renderElement(centerPopover(ftxui::text("Popup") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 5)), 21, 7);

    auto const optPopupBox = findTextCells(rendered.screen, "Popup");

    REQUIRE(optPopupBox);
    CHECK(optPopupBox->x_min == 8);
    CHECK(optPopupBox->y_min == 3);
  }

  TEST_CASE("Render - center popover clears a one-cell halo", "[tui][unit][render]")
  {
    auto rows = ftxui::Elements{};

    for (std::int32_t row = 0; row < 7; ++row)
    {
      rows.push_back(ftxui::text(std::string(21, '#')));
    }

    auto const rendered =
      renderElement(ftxui::dbox({
                      ftxui::vbox(std::move(rows)),
                      centerPopover(ftxui::text("Popup") | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 5)),
                    }),
                    21,
                    7);

    CHECK(rendered.screen.PixelAt(7, 3).character == " ");
    CHECK(rendered.screen.PixelAt(13, 3).character == " ");
    CHECK(rendered.screen.PixelAt(8, 2).character == " ");
    CHECK(rendered.screen.PixelAt(8, 4).character == " ");
    CHECK(rendered.screen.PixelAt(6, 3).character == "#");
    CHECK(rendered.screen.PixelAt(14, 3).character == "#");
  }

  TEST_CASE("Render - titled panel places workspace context on the lower edge", "[tui][unit][render]")
  {
    auto listBox = ftxui::Box{};
    auto viewBox = ftxui::Box{};
    auto const rendered =
      renderElement(style::titledPanel(
                      "",
                      ftxui::text("Body"),
                      style::PanelOptions{
                        .leftFooter = style::PanelEdgeButton{.label = "list", .value = "All Tracks", .box = &listBox},
                        .leftFooterRight = style::PanelEdgeButton{.label = "view", .value = "albums", .box = &viewBox},
                        .rightFooter = "3 / 8 tracks"}),
                    64,
                    5);

    auto const optListLabelBox = findTextCells(rendered.screen, "list");
    auto const optListValueBox = findTextCells(rendered.screen, "All Tracks");
    auto const optViewLabelBox = findTextCells(rendered.screen, "view");
    auto const optViewValueBox = findTextCells(rendered.screen, "albums");
    auto const optFooterBox = findTextCells(rendered.screen, "3 / 8 tracks");
    REQUIRE(optListLabelBox);
    REQUIRE(optListValueBox);
    REQUIRE(optViewLabelBox);
    REQUIRE(optViewValueBox);
    REQUIRE(optFooterBox);
    CHECK_FALSE(rendered.text.contains("view:"));
    CHECK(optListLabelBox->y_min == 4);
    CHECK(optListValueBox->y_min == 4);
    CHECK(optViewLabelBox->y_min == 4);
    CHECK(optViewValueBox->y_min == 4);
    CHECK(listBox.y_min == 4);
    CHECK(listBox.x_min <= optListLabelBox->x_min);
    CHECK(listBox.x_max >= optListValueBox->x_max);
    CHECK(viewBox.y_min == 4);
    CHECK(viewBox.x_min <= optViewLabelBox->x_min);
    CHECK(viewBox.x_max >= optViewValueBox->x_max);
    CHECK(rendered.screen.PixelAt(1, 0).character == "─");
    CHECK(rendered.screen.PixelAt(0, 4).character == "╰");
    CHECK(rendered.screen.PixelAt(1, 4).character == "─");
    auto const checkFrameEdgePixel = [&](std::int32_t const column)
    {
      auto const pixel = rendered.screen.PixelAt(column, 4);
      CHECK(pixel.foreground_color == ftxui::Color::Default);
      CHECK_FALSE(pixel.bold);
      CHECK_FALSE(pixel.dim);
    };
    CHECK(rendered.screen.PixelAt(optListValueBox->x_max + 1, 4).character == " ");
    CHECK(rendered.screen.PixelAt(optListValueBox->x_max + 2, 4).character == "─");
    CHECK(rendered.screen.PixelAt(optListValueBox->x_max + 3, 4).character == " ");
    checkFrameEdgePixel(optListValueBox->x_max + 1);
    checkFrameEdgePixel(optListValueBox->x_max + 2);
    checkFrameEdgePixel(optListValueBox->x_max + 3);
    CHECK(rendered.screen.PixelAt(optListLabelBox->x_min, 4).dim);
    CHECK(rendered.screen.PixelAt(optViewLabelBox->x_min, 4).dim);
    CHECK(rendered.screen.PixelAt(optListValueBox->x_min, 4).foreground_color == ftxui::Color::Cyan);
    CHECK(rendered.screen.PixelAt(optViewValueBox->x_min, 4).foreground_color == ftxui::Color::Cyan);
    CHECK(optFooterBox->y_min == 4);
    CHECK(rendered.screen.PixelAt(optFooterBox->x_min - 2, 4).character == "─");
    checkFrameEdgePixel(optFooterBox->x_min - 2);
    checkFrameEdgePixel(optFooterBox->x_min - 1);
    CHECK(rendered.screen.PixelAt(62, 4).character == "─");
    CHECK(rendered.screen.PixelAt(63, 4).character == "╯");
  }

  TEST_CASE("Render - hovered edge button uses interactive surface without tinting frame separators",
            "[tui][unit][render]")
  {
    auto const rendered =
      renderElement(style::titledPanel(
                      "",
                      ftxui::text("Body"),
                      style::PanelOptions{
                        .leftFooter = style::PanelEdgeButton{.label = "list", .value = "All Tracks", .hovered = true},
                        .leftFooterRight = style::PanelEdgeButton{.label = "view", .value = "albums"}}),
                    64,
                    5);

    auto const optListLabelBox = findTextCells(rendered.screen, "list");
    auto const optListValueBox = findTextCells(rendered.screen, "All Tracks");
    auto const optViewValueBox = findTextCells(rendered.screen, "albums");
    REQUIRE(optListLabelBox);
    REQUIRE(optListValueBox);
    REQUIRE(optViewValueBox);

    auto const hoveredPixel = rendered.screen.PixelAt(optListLabelBox->x_min, optListLabelBox->y_min);
    checkInteractiveSurface(hoveredPixel);
    CHECK(rendered.screen.PixelAt(optListValueBox->x_min, optListValueBox->y_min).background_color ==
          ftxui::Color::Yellow);
    CHECK(rendered.screen.PixelAt(optListValueBox->x_max + 2, optListValueBox->y_min).foreground_color ==
          ftxui::Color::Default);
    CHECK(rendered.screen.PixelAt(optListValueBox->x_max + 2, optListValueBox->y_min).background_color ==
          ftxui::Color::Default);
    CHECK(rendered.screen.PixelAt(optViewValueBox->x_min, optViewValueBox->y_min).foreground_color ==
          ftxui::Color::Cyan);
    CHECK(rendered.screen.PixelAt(optViewValueBox->x_min, optViewValueBox->y_min).background_color ==
          ftxui::Color::Default);
  }

  TEST_CASE("Render - titled panel body padding is opt-in for popovers", "[tui][unit][render]")
  {
    auto const unpadded = renderElement(style::titledPanel("Panel", ftxui::text("Body")), 16, 3);
    auto const padded = renderElement(style::popupPanel("Panel", ftxui::text("Body")), 16, 3);

    auto const optUnpaddedBody = findTextCells(unpadded.screen, "Body");
    auto const optPaddedBody = findTextCells(padded.screen, "Body");

    REQUIRE(optUnpaddedBody);
    REQUIRE(optPaddedBody);
    CHECK(optUnpaddedBody->x_min == 1);
    CHECK(optPaddedBody->x_min == 2);
  }

  TEST_CASE("Render - expanded status bar advertises frequent workspace actions", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    auto const rendered = renderElement(statusBar(StatusBarViewState{.shell = &shell}), 140, 1);

    CHECK_FALSE(rendered.text.contains("Ready"));
    CHECK_FALSE(rendered.text.contains("3 / 8 tracks"));
    CHECK(lineIndexContaining(rendered.text, "/ Filter") == 0);
    CHECK(rendered.text.contains("/ Filter"));
    CHECK(rendered.text.contains(": command"));
    CHECK(rendered.text.contains("l lists"));
    CHECK(rendered.text.contains("v view"));
    CHECK(rendered.text.contains("d detail"));
    CHECK(rendered.text.contains("? help"));
    CHECK_FALSE(rendered.text.contains("n notif"));
    CHECK_FALSE(rendered.text.contains("a pipeline"));
    CHECK_FALSE(rendered.text.contains("o output"));
    CHECK_FALSE(rendered.text.contains("groups"));
    CHECK_FALSE(rendered.text.contains("Ctrl-L current"));
    CHECK_FALSE(rendered.text.contains("q quit"));
    CHECK_FALSE(rendered.text.contains("c clear filter"));
    CHECK_FALSE(rendered.text.contains("Mode:"));
    CHECK_FALSE(rendered.text.contains("Filter:"));
    CHECK_FALSE(rendered.text.contains("view:"));

    auto const optShortcutBox = findTextCells(rendered.screen, "/ Filter");
    REQUIRE(optShortcutBox);
    CHECK(optShortcutBox->x_min > 0);
    CHECK_FALSE(rendered.text.contains("│"));

    auto const shortcutPixel = rendered.screen.PixelAt(optShortcutBox->x_min, optShortcutBox->y_min);
    CHECK(shortcutPixel.foreground_color == ftxui::Color::Cyan);
    CHECK(shortcutPixel.bold);
  }

  TEST_CASE("Render - compact status bar keeps only input and help entry points", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    auto const rendered = renderElement(statusBar(StatusBarViewState{.terminalColumns = 80, .shell = &shell}), 80, 1);

    CHECK_FALSE(rendered.text.contains("Ready"));
    CHECK_FALSE(rendered.text.contains("3 / 8 tracks"));
    CHECK(lineIndexContaining(rendered.text, "/ Filter") == 0);
    CHECK(rendered.text.contains(": command"));
    CHECK(rendered.text.contains("? help"));
    CHECK_FALSE(rendered.text.contains("l lists"));
    CHECK_FALSE(rendered.text.contains("v view"));
    CHECK_FALSE(rendered.text.contains("d detail"));
    CHECK_FALSE(rendered.text.contains("q quit"));
  }

  TEST_CASE("Render - status bar shows filter only when applied", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    auto const rendered = renderText(statusBar(StatusBarViewState{.filterDraft = "Aimer", .shell = &shell}), 140);

    CHECK(rendered.contains("/ Aimer"));
    CHECK(rendered.contains("c clear filter"));
    CHECK_FALSE(rendered.contains("/ Filter"));
    CHECK_FALSE(rendered.contains("Filter:"));
  }

  TEST_CASE("Render - status bar uses overlay-specific help for every overlay", "[tui][unit][render]")
  {
    struct Case final
    {
      Overlay overlay = Overlay::None;
      std::string_view label{};
      std::string_view hint{};
    };

    auto const cases = std::vector<Case>{
      {.overlay = Overlay::ListChooser, .label = "Lists", .hint = "l toggle  Enter open  Esc close"},
      {.overlay = Overlay::DetailPanel, .label = "Detail", .hint = "d toggle  Esc close"},
      {.overlay = Overlay::QualityPanel, .label = "Pipeline", .hint = "a toggle  Esc close"},
      {.overlay = Overlay::OutputDevices, .label = "Output", .hint = "o toggle  Enter select  Esc close"},
      {.overlay = Overlay::PresentationPanel, .label = "Views", .hint = "v toggle  Enter select  Esc close"},
      {.overlay = Overlay::Notifications, .label = "Notifications", .hint = "n toggle  x hide compact  Esc close"},
      {.overlay = Overlay::Help, .label = "Help", .hint = "Esc close"},
    };

    for (auto const& item : cases)
    {
      auto shell = ShellInteractionModel{};
      shell.openOverlay(item.overlay);

      auto const rendered = renderText(statusBar(StatusBarViewState{.shell = &shell}));

      CHECK(rendered.contains(item.label));
      CHECK(rendered.contains(item.hint));
      CHECK_FALSE(rendered.contains("/ command"));
    }
  }

  TEST_CASE("Render - status slot shows activity compact state", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    auto activity = uimodel::ActivityStatusViewState{.compact = uimodel::ActivityCompactState{
                                                       .kind = uimodel::ActivityStatusKind::Warning,
                                                       .text = "Partial import",
                                                       .dismissible = true,
                                                       .hasDetails = true,
                                                     }};
    auto activityBox = ftxui::Box{};

    auto const rendered = renderElement(
      statusBar(StatusBarViewState{.activityStatus = &activity, .shell = &shell, .activityStatusBox = &activityBox}),
      180,
      1);

    CHECK(rendered.text.contains("warn"));
    CHECK(rendered.text.contains("Partial import"));
    CHECK_FALSE(rendered.text.contains("Ready"));
    CHECK(activityBox.x_min == 0);
    CHECK(activityBox.y_min == 0);
  }

  TEST_CASE("Render - idle status bar clears stale activity hit box", "[tui][regression][render]")
  {
    auto shell = ShellInteractionModel{};
    auto activity = uimodel::ActivityStatusViewState{.compact = uimodel::ActivityCompactState{
                                                       .kind = uimodel::ActivityStatusKind::Info,
                                                       .text = "Ready",
                                                       .dismissible = true,
                                                     }};
    auto activityBox = ftxui::Box{};

    renderElement(
      statusBar(StatusBarViewState{.activityStatus = &activity, .shell = &shell, .activityStatusBox = &activityBox}),
      180,
      1);
    REQUIRE(hasHitArea(activityBox));

    activity = uimodel::ActivityStatusViewState{};
    renderElement(
      statusBar(StatusBarViewState{.activityStatus = &activity, .shell = &shell, .activityStatusBox = &activityBox}),
      180,
      1);

    CHECK_FALSE(hasHitArea(activityBox));
  }

  TEST_CASE("Render - hovered status slot uses a readable interactive surface", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    auto activity = uimodel::ActivityStatusViewState{.compact = uimodel::ActivityCompactState{
                                                       .kind = uimodel::ActivityStatusKind::Warning,
                                                       .text = "Partial import",
                                                       .dismissible = true,
                                                       .hasDetails = true,
                                                     }};

    auto const rendered = renderElement(
      statusBar(StatusBarViewState{.activityStatus = &activity, .shell = &shell, .activityStatusHovered = true}),
      180,
      1);

    auto const optWarnBox = findTextCells(rendered.screen, "warn");
    REQUIRE(optWarnBox);
    auto const pixel = rendered.screen.PixelAt(optWarnBox->x_min, optWarnBox->y_min);
    checkInteractiveSurface(pixel);
  }

  TEST_CASE("Render - notification center lists clearable notification details", "[tui][unit][render]")
  {
    auto state = uimodel::ActivityStatusViewState{
      .compact =
        uimodel::ActivityCompactState{
          .kind = uimodel::ActivityStatusKind::Error,
          .text = "Scan failed",
          .dismissible = true,
          .hasDetails = true,
        },
      .detail = uimodel::ActivityDetailState{
        .items = {uimodel::ActivityDetailItem{.id = rt::NotificationId{9},
                                              .severity = rt::NotificationSeverity::Error,
                                              .message = "Permission denied",
                                              .dismissible = true}},
      }};
    auto rowHitRegions = std::vector<NotificationDetailRowHitRegion>{};

    auto const rendered = renderElement(notificationCenterPanel(state, &rowHitRegions), 64, 12);

    CHECK(rendered.text.contains("Notifications"));
    CHECK(rendered.text.contains("Scan failed"));
    CHECK(rendered.text.contains("Permission denied"));
    CHECK(rendered.text.contains("click clearable row"));
    REQUIRE(rowHitRegions.size() == 1);
    CHECK(rowHitRegions.front().id == rt::NotificationId{9});
    CHECK(rowHitRegions.front().dismissible);
    CHECK(rowHitRegions.front().box.y_min > 0);
  }

  TEST_CASE("Render - text input leaves bottom status bar in workspace layout", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "view albums");

    auto const rendered = renderText(statusBar(StatusBarViewState{.terminalColumns = 100, .shell = &shell}));

    CHECK_FALSE(rendered.contains("Command"));
    CHECK_FALSE(rendered.contains(":view albums"));
    CHECK_FALSE(rendered.contains("Tab complete"));
    CHECK(rendered.contains("/ Filter"));
    CHECK(rendered.contains(": command"));
  }

  TEST_CASE("Render - command palette keeps ratio-sized dimensions stable across completion content",
            "[tui][unit][render]")
  {
    auto const wideColumns = commandPalettePanelColumns(180);
    CHECK(commandPalettePanelColumns(0) == 72);
    CHECK(wideColumns == 72);
    CHECK(commandPalettePanelColumns(100) == 56);
    CHECK(commandPalettePanelColumns(40) == 40);
    CHECK(commandPalettePanelRows(0) == 18);
    CHECK(commandPalettePanelRows(30) == 12);
    CHECK(commandPalettePanelRows(50) == 18);
    CHECK(commandPalettePanelRows(80) == 20);
    CHECK(commandPalettePanelRows(8) == 8);

    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "a");
    shell.setCommandCompletion(rt::CompletionResult{
      .replaceBegin = 0,
      .replaceEnd = 1,
      .items = {rt::CompletionItem{.displayText = "a very long completion label that should not resize the panel",
                                   .insertText = "a very long completion label that should not resize the panel",
                                   .detail = rt::CompletionDetail::makeResolvedText(
                                     "a long detail that should also stay inside the fixed ratio frame")}},
    });

    CHECK(commandPalettePanelColumns(180) == wideColumns);
  }

  TEST_CASE("Render - command palette keeps selected completion visible in constrained height",
            "[tui][regression][render]")
  {
    auto shell = ShellInteractionModel{};
    auto items = std::vector<rt::CompletionItem>{};

    for (std::int32_t index = 0; index < 8; ++index)
    {
      auto value = std::format("Option {}", index);
      items.push_back(rt::CompletionItem{
        .displayText = value, .insertText = value, .detail = rt::CompletionDetail::makeResolvedText("item")});
    }

    shell.beginInput(ShellInputMode::Command, "o");
    shell.setCommandCompletion(rt::CompletionResult{.replaceBegin = 0, .replaceEnd = 1, .items = std::move(items)});

    for (std::int32_t index = 0; index < 7; ++index)
    {
      REQUIRE(shell.moveCommandCompletion(1));
    }

    auto const rendered =
      renderElement(commandPalettePanel(shell, 48) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 12), 48, 12);

    CHECK(rendered.text.contains("Option 7"));
    CHECK(rendered.text.contains("Tab complete"));
  }

  TEST_CASE("Render - Quick Filter moves input to the status bar and keeps completions in its popup",
            "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::QuickFilter, "A");
    shell.setCommandCompletion(rt::CompletionResult{
      .replaceBegin = 0,
      .replaceEnd = 1,
      .items = {rt::CompletionItem{
        .displayText = "Aimer", .insertText = "Aimer", .detail = rt::CompletionDetail::makeResolvedText("artist")}},
    });

    auto const status = renderText(statusBar(StatusBarViewState{.shell = &shell}));
    auto const popup = renderText(quickFilterCompletionPanel(shell));

    CHECK(status.contains("/ Aimer_"));
    CHECK_FALSE(status.contains("/ Filter"));
    CHECK_FALSE(status.contains(": command"));
    CHECK(popup.contains("Quick Filter"));
    CHECK(popup.contains("Aimer"));
    CHECK(popup.contains("Enter accept"));
    CHECK(popup.contains("Esc keep typed"));
  }

  TEST_CASE("Render - Quick Filter popup shows the current expression error", "[tui][unit][render][filter]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::QuickFilter, "$artist =");

    auto const rendered = renderText(quickFilterCompletionPanel(shell, 56, "Filter error: expected value"));

    CHECK(rendered.contains("Quick Filter"));
    CHECK(rendered.contains("Filter error: expected value"));
    CHECK(rendered.contains("Esc keep typed"));
  }

  TEST_CASE("Render - Quick Filter popup height follows results and leaves the status row visible",
            "[tui][unit][render][filter]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::QuickFilter, "a");
    shell.setCommandCompletion(rt::CompletionResult{
      .replaceBegin = 0,
      .replaceEnd = 1,
      .items =
        {
          rt::CompletionItem{.displayText = "Aimer", .insertText = "Aimer"},
          rt::CompletionItem{.displayText = "Adele", .insertText = "Adele"},
        },
    });

    CHECK(quickFilterPanelRows(shell, false, 24) == 6);
    CHECK(quickFilterPanelRows(shell, true, 24) == 8);
    CHECK(quickFilterPanelRows(shell, true, 5) == 4);
  }

  TEST_CASE("Render - command palette renders without completion matches", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command, "view albums");

    auto const rendered = renderText(commandPalettePanel(shell));

    CHECK(rendered.contains(":view albums"));
    CHECK(rendered.contains("No matches"));
    CHECK(rendered.contains("Enter run"));
  }

  TEST_CASE("Render - command palette keeps empty input separate from suggestions", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command);
    shell.setCommandCompletion(rt::CompletionResult{
      .replaceBegin = 0,
      .replaceEnd = 0,
      .items = {rt::CompletionItem{.displayText = ":output",
                                   .insertText = "output",
                                   .detail = rt::CompletionDetail::makeResolvedText("output device")}},
    });

    auto const rendered = renderText(commandPalettePanel(shell));

    CHECK(rendered.contains(":_"));
    CHECK(rendered.contains(":output"));
    CHECK(rendered.contains("Tab complete"));
  }

  TEST_CASE("Render - command palette renders completion metadata and fallback details", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command);
    shell.setCommandCompletion(rt::CompletionResult{
      .items =
        {
          rt::CompletionItem{.displayText = ":view",
                             .insertText = "view ",
                             .detail = rt::CompletionDetail::makeResolvedText("track view")},
          rt::CompletionItem{
            .displayText = "Aimer", .insertText = "Aimer", .detail = rt::CompletionDetail::makeResolvedText("artist")},
        },
    });
    REQUIRE(shell.moveCommandCompletion(1));

    auto const rendered = renderElement(commandPalettePanel(shell, 48), 48, 8);

    CHECK(rendered.text.contains("Command Palette"));
    CHECK(rendered.text.contains(":view"));

    // A command reachable by key shows that key where its detail would go, so
    // the assertion has to be that the detail is gone. Checking for a 'v'
    // instead passes on the word ":view" itself and would survive the hint
    // being dropped entirely.
    CHECK_FALSE(rendered.text.contains("track view"));

    // An item that is not a command keeps its detail.
    CHECK(rendered.text.contains("Aimer"));
    CHECK(rendered.text.contains("artist"));
    auto const optSelected = findTextCells(rendered.screen, "Aimer");
    REQUIRE(optSelected);
    CHECK_FALSE(rendered.screen.PixelAt(optSelected->x_min, optSelected->y_min).inverted);
    checkInteractiveSurface(rendered.screen.PixelAt(optSelected->x_min, optSelected->y_min));
  }

  TEST_CASE("Render - command palette does not infer metadata for non-command items", "[tui][unit][render]")
  {
    auto shell = ShellInteractionModel{};
    shell.beginInput(ShellInputMode::Command);
    shell.setCommandCompletion(rt::CompletionResult{
      .items = {rt::CompletionItem{
        .displayText = "v", .insertText = "v", .detail = rt::CompletionDetail::makeResolvedText("artist")}},
    });

    auto const rendered = renderElement(commandPalettePanel(shell, 32), 32, 8);

    CHECK(rendered.text.contains('v'));
    CHECK(rendered.text.contains("artist"));
    CHECK_FALSE(rendered.text.contains("/v"));
    CHECK_FALSE(rendered.text.contains("view"));
  }

  TEST_CASE("Render - presentation panel renders selected and active views", "[tui][unit][render]")
  {
    auto const items = std::vector<TrackPresentationNavEntry>{
      {.id = "songs", .label = "Songs", .detail = "General-purpose song list."},
      {.id = "albums", .label = "Albums", .detail = "Grouped by album."},
    };
    auto rowHitRegions = std::vector<PresentationRowHitRegion>{};

    auto const rendered = renderElement(presentationPanel(items, "albums", 1, &rowHitRegions), 48, 16);

    CHECK(rendered.text.contains("Views"));
    CHECK(rendered.text.contains("albums"));
    CHECK(rendered.text.contains("* Albums"));
    CHECK(rendered.text.contains("Grouped by album."));
    CHECK_FALSE(rendered.text.contains("songs"));
    REQUIRE(rowHitRegions.size() == 2);
    CHECK(rowHitRegions[1].rowIndex == 1);
    CHECK_FALSE(rendered.screen.PixelAt(rowHitRegions[1].box.x_min, rowHitRegions[1].box.y_min).inverted);
    CHECK(rendered.screen.PixelAt(rowHitRegions[1].box.x_min, rowHitRegions[1].box.y_min).foreground_color ==
          ftxui::Color::Black);
    CHECK(rendered.screen.PixelAt(rowHitRegions[1].box.x_min, rowHitRegions[1].box.y_min).background_color ==
          ftxui::Color::Yellow);
  }

  TEST_CASE("Render - presentation panel width follows content and terminal bounds", "[tui][unit][render]")
  {
    auto const items = std::vector<TrackPresentationNavEntry>{
      {.id = "wide",
       .label = "Wide View",
       .detail = "Detailed description long enough to widen the picker beyond the default."},
    };

    auto const wideColumns = presentationPanelColumns(items, "wide", 120);

    CHECK(wideColumns > kPresentationPanelColumns);
    CHECK(wideColumns <= 120);
    CHECK(presentationPanelColumns(items, "wide", 60) == 60);
    CHECK(presentationPanelColumns(std::vector{TrackPresentationNavEntry{.id = "x", .label = "X"}}, "x", 120) <
          kPresentationPanelColumns);

    auto const rendered = renderElement(presentationPanel(items, "wide", 0, nullptr, wideColumns), wideColumns, 16);

    CHECK(rendered.text.contains("Detailed description"));
  }

  TEST_CASE("Render - presentation panel handles empty and out-of-range selection", "[tui][unit][render]")
  {
    auto rowHitRegions = std::vector<PresentationRowHitRegion>{};
    auto const emptyRendered =
      renderElement(presentationPanel(std::vector<TrackPresentationNavEntry>{}, "", 99, &rowHitRegions), 48, 16);

    CHECK(emptyRendered.text.contains("default"));
    CHECK(emptyRendered.text.contains("No views available"));
    CHECK(rowHitRegions.empty());

    auto const items = std::vector<TrackPresentationNavEntry>{
      {.id = "songs", .label = "Songs", .detail = "General-purpose song list."},
    };
    auto const rendered = renderElement(presentationPanel(items, "songs", 99, &rowHitRegions), 48, 16);

    REQUIRE(rowHitRegions.size() == 1);
    CHECK_FALSE(rendered.screen.PixelAt(rowHitRegions[0].box.x_min, rowHitRegions[0].box.y_min).inverted);
  }
} // namespace ao::tui::test
