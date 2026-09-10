// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/EventControllerTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/HitRegions.h"
#include "tui/Keymap.h"
#include "tui/NavigationPanel.h"
#include "tui/Render.h"
#include "tui/ShellInteractionModel.h"
#include "tui/StatusBar.h"
#include "tui/TextCell.h"
#include "tui/TrackListEntry.h"
#include <ao/AudioCodec.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackRow.h>
#include <ao/uimodel/input/KeymapModel.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <array>
#include <string>

namespace ao::tui::test
{
  TEST_CASE("DetailSections - explicit focus stays outside the normal panel cycle", "[tui][regression][detail]")
  {
    using namespace ftxui;
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.hitRegions.navigationLayout = navigationGeometry(140, 0, true);
    REQUIRE(events.tryHandleEvent(Event::Character('d')));
    CHECK(fixture.shell.isDetailVisible());
    CHECK_FALSE(fixture.shell.isDetailFocused());
    REQUIRE(events.tryHandleEvent(Event::Tab));
    CHECK(fixture.shell.isNavigationFocused());
    REQUIRE(events.tryHandleEvent(Event::Tab));
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    CHECK_FALSE(fixture.shell.isDetailFocused());
    REQUIRE(events.tryHandleEvent(Event::Character('D')));
    CHECK(fixture.shell.isDetailFocused());
    REQUIRE(events.tryHandleEvent(Event::Character('j')));
    CHECK(fixture.shell.detailSections().selected == 1);
    CHECK(library.selectedTrack() == 0);
    REQUIRE(events.tryHandleEvent(Event::Return));
    CHECK(fixture.shell.detailSections().expanded[1]);
    REQUIRE(events.tryHandleEvent(Event::ArrowLeft));
    CHECK_FALSE(fixture.shell.detailSections().expanded[1]);
    REQUIRE(events.tryHandleEvent(Event::ArrowRight));
    CHECK(fixture.shell.detailSections().expanded[1]);
    REQUIRE(events.tryHandleEvent(Event::Escape));
    CHECK_FALSE(fixture.shell.isDetailFocused());
    CHECK(fixture.shell.isDetailVisible());
    REQUIRE(events.tryHandleEvent(Event::Character('j')));
    CHECK(library.selectedTrack() == 1);
    REQUIRE(events.tryHandleEvent(Event::Character('D')));
    REQUIRE(events.tryHandleEvent(Event::TabReverse));
    CHECK_FALSE(fixture.shell.isDetailFocused());
    CHECK_FALSE(fixture.shell.isNavigationFocused());
    REQUIRE(events.tryHandleEvent(Event::Character('D')));
    REQUIRE(events.tryHandleEvent(Event::Character('d')));
    CHECK_FALSE(fixture.shell.isDetailFocused());
    CHECK_FALSE(fixture.shell.isDetailVisible());
    REQUIRE(events.tryHandleEvent(Event::Character('D')));
    CHECK(fixture.shell.isDetailVisible());
    CHECK(fixture.shell.detailSections().expanded[1]);
    fixture.shell.setNavigationPinned(false);
    fixture.shell.reconcileNavigationLayout(false);
    CHECK(fixture.shell.isDetailFocused());
  }

  TEST_CASE("DetailSections - playback remains available while section keys leave track selection alone",
            "[tui][regression][detail]")
  {
    using namespace ftxui;
    auto fixture = EventControllerFixture{};
    fixture.addReadyAudioProvider();
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    REQUIRE(events.tryHandleEvent(Event::Character('D')));
    REQUIRE(events.tryHandleEvent(Event::Character('j')));
    REQUIRE(events.tryHandleEvent(Event::Character(' ')));
    REQUIRE(fixture.tryWaitForPlayback(library.tracks().front().id));
    CHECK(library.selectedTrack() == 0);
    CHECK(fixture.shell.isDetailFocused());
    CHECK(fixture.shell.detailSections().selected == 1);
  }

  TEST_CASE("DetailSections - input and modals own keys before detail and focus can be rebound",
            "[tui][regression][detail]")
  {
    using namespace ftxui;
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto keymap = uimodel::KeymapModel{defaultKeymap()};
    keymap.applyOverrides({{"tui.detail.focus", {"F2"}}});
    auto const plan = KeymapPlan{keymap};
    auto events = fixture.makeEvents(library, plan);
    REQUIRE(events.tryHandleEvent(Event::F2));
    CHECK(fixture.shell.isDetailFocused());
    fixture.shell.openOverlay(Overlay::Help);
    REQUIRE(events.tryHandleEvent(Event::Escape));
    CHECK(fixture.shell.overlay() == Overlay::None);
    CHECK(fixture.shell.isDetailFocused());
    fixture.shell.beginInput(ShellInputMode::QuickFilter);
    REQUIRE(events.tryHandleEvent(Event::Character('D')));
    CHECK(fixture.shell.inputDraft() == "D");
    fixture.shell.closeInput();
    REQUIRE(events.tryHandleEvent(Event::Escape));
    CHECK_FALSE(fixture.shell.isDetailFocused());
  }

  TEST_CASE("DetailSections - visible header clicks fold without taking focus and reject stale tracks",
            "[tui][regression][detail][mouse]")
  {
    using namespace ftxui;
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.shell.toggleDetail();
    fixture.shell.focusNavigation();
    fixture.hitRegions.navigationLayout = navigationGeometry(140, 0, true);
    auto const* track = library.selectedTrackView().track;
    REQUIRE(track != nullptr);
    fixture.hitRegions.detailTrack = track->id;
    renderElement(
      detailPane(ao::test::englishMessageCatalog(),
                 track,
                 {},
                 40,
                 &fixture.hitRegions.detailPanel,
                 0,
                 {.sections = fixture.shell.detailSections(), .headerBoxes = &fixture.hitRegions.detailSectionBoxes}),
      40,
      20);
    auto const box = fixture.hitRegions.detailSectionBoxes[0];
    auto const click = clickBox(box);
    REQUIRE(events.tryHandleEvent(click));
    CHECK_FALSE(fixture.shell.detailSections().expanded[0]);
    CHECK(fixture.shell.isNavigationFocused());
    CHECK_FALSE(fixture.shell.isDetailFocused());
    library.moveTrackSelection(1);
    REQUIRE(events.tryHandleEvent(click));
    CHECK_FALSE(fixture.shell.detailSections().expanded[0]);
    CHECK(fixture.shell.isNavigationFocused());
  }

  TEST_CASE("DetailSections - focused status preserves live activity and replaces stale hit regions",
            "[tui][regression][detail][mouse]")
  {
    auto fixture = EventControllerFixture{};
    auto library = fixture.makeLibrary();
    auto events = fixture.makeEvents(library);
    fixture.runtimePtr->notifications().post(
      rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::pinned());
    fixture.shell.focusDetail();
    fixture.hitRegions.activityStatusBox = {.x_min = 120, .x_max = 139, .y_min = 5, .y_max = 5};
    auto render = [&]
    {
      return renderElement(statusBar(ao::test::englishMessageCatalog(),
                                     {.activityStatus = &fixture.activityStatusViewModel.viewState(),
                                      .terminalColumns = 80,
                                      .shell = &fixture.shell,
                                      .activityStatusBox = &fixture.hitRegions.activityStatusBox},
                                     defaultKeymapPlan()),
                           80,
                           1);
    };
    auto const shown = render();
    CHECK(shown.text.contains("Partial import"));
    CHECK(shown.text.contains("Esc tracks"));
    auto const french = renderElement(statusBar(ao::test::messageCatalog("fr-FR"),
                                                {.activityStatus = &fixture.activityStatusViewModel.viewState(),
                                                 .terminalColumns = 100,
                                                 .shell = &fixture.shell},
                                                defaultKeymapPlan()),
                                      100,
                                      1);
    CHECK(french.text.contains("Esc titres"));
    CHECK(french.text.contains("Partial import"));
    auto const optWarning = findTextCells(shown.screen, "Partial import");
    REQUIRE(optWarning);
    CHECK(contains(fixture.hitRegions.activityStatusBox, optWarning->x_min, optWarning->y_min));
    CHECK_FALSE(contains(fixture.hitRegions.activityStatusBox, 120, 5));
    REQUIRE(events.tryHandleEvent(ftxui::Event::Mouse("",
                                                      {.button = ftxui::Mouse::Left,
                                                       .motion = ftxui::Mouse::Pressed,
                                                       .x = optWarning->x_min,
                                                       .y = optWarning->y_min})));
    CHECK(fixture.shell.overlay() == Overlay::Notifications);
    REQUIRE(events.tryHandleEvent(ftxui::Event::Escape));
    CHECK(fixture.shell.isDetailFocused());
    fixture.activityStatusViewModel.dismissCompact();
    render();
    CHECK(fixture.hitRegions.activityStatusBox.IsEmpty());
  }

  TEST_CASE("DetailSections - full hints reserve the standard activity width at the boundary",
            "[tui][regression][detail][render]")
  {
    auto fixture = EventControllerFixture{};
    fixture.runtimePtr->notifications().post(
      rt::NotificationSeverity::Warning, "Partial import", rt::NotificationLifetime::pinned());
    fixture.shell.focusDetail();

    for (auto const* locale : {"en", "zh-Hant", "fr", "es"})
    {
      auto const catalog = ao::test::messageCatalog(locale);
      auto const full = i18n::requiredText(catalog, i18n::MessageId::TuiDetailKeys);
      auto const compact = i18n::requiredText(catalog, i18n::MessageId::TuiDetailKeysCompact);

      for (auto const activityColumns : {23, 24})
      {
        auto const columns = cellWidth(full) + activityColumns;
        auto const rendered = renderElement(statusBar(catalog,
                                                      {.activityStatus = &fixture.activityStatusViewModel.viewState(),
                                                       .terminalColumns = columns,
                                                       .shell = &fixture.shell},
                                                      defaultKeymapPlan()),
                                            columns,
                                            1);
        auto const expectedHint = activityColumns == 24 ? full : compact;
        auto const optHintBox = findTextCells(rendered.screen, expectedHint);
        auto const optActivityBox = findTextCells(rendered.screen, "Partial import");
        REQUIRE(optHintBox);
        REQUIRE(optActivityBox);
        CHECK(optActivityBox->x_max < optHintBox->x_min);
        CHECK(optHintBox->x_min + cellWidth(expectedHint) == columns);

        if (activityColumns == 23)
        {
          CHECK_FALSE(rendered.text.contains(full));
        }
      }
    }
  }

  TEST_CASE("DetailSections - tags belong to Metadata independently of Audio Properties",
            "[tui][regression][detail][render]")
  {
    auto const& catalog = ao::test::englishMessageCatalog();
    auto const track =
      makeTrackListEntry(catalog, rt::TrackRow{.title = "Title", .tags = "favourite", .codec = AudioCodec::Flac});

    for (bool const metadataExpanded : {false, true})
    {
      for (bool const audioExpanded : {false, true})
      {
        auto headers = std::array<ftxui::Box, 2>{};
        auto const rendered = renderElement(
          detailPane(catalog,
                     &track,
                     {},
                     40,
                     nullptr,
                     0,
                     {.sections = {.expanded = {metadataExpanded, audioExpanded}}, .headerBoxes = &headers}),
          40,
          20);
        auto const optTags = findTextCells(rendered.screen, "favourite");
        CHECK(optTags.has_value() == metadataExpanded);

        if (metadataExpanded)
        {
          REQUIRE(optTags);
          CHECK(optTags->y_min > headers[0].y_max);
          CHECK(optTags->y_max < headers[1].y_min);
        }
      }
    }
  }

  TEST_CASE("DetailSections - selecting an offscreen header reveals it and folding preserves identity",
            "[tui][regression][detail][render]")
  {
    auto row = rt::TrackRow{.title = "Title",
                            .artist = "Artist",
                            .composer = std::string(400, 'x'),
                            .tags = "favourite",
                            .sampleRate = 44100,
                            .channels = 2,
                            .bitDepth = 16,
                            .codec = AudioCodec::Flac};
    auto const track = makeTrackListEntry(ao::test::englishMessageCatalog(), row);
    auto regions = PanelMouseRegions{};
    auto headers = std::array<ftxui::Box, 2>{};
    auto options = DetailPaneOptions{.headerBoxes = &headers, .focused = true};
    auto render = [&]
    {
      return renderElement(detailPane(ao::test::englishMessageCatalog(), &track, {}, 40, &regions, 0, options), 40, 10);
    };
    auto const top = render();
    CHECK(top.text.contains("Metadata"));
    CHECK(headers[1].y_min > regions.navigationBox.y_max);
    options.sections.selected = 1;
    options.sections.revealSelected = true;
    auto const audio = render();
    CHECK(headers[1].y_min >= regions.navigationBox.y_min);
    CHECK(headers[1].y_max <= regions.navigationBox.y_max);
    CHECK(audio.text.contains("FLAC"));
    checkInteractiveSurface(audio.screen.PixelAt(headers[1].x_min, headers[1].y_min));
    options.sections.selected = headers.size();
    auto const invalidSelection = render();
    CHECK(invalidSelection.text.contains("Metadata"));
    CHECK(headers[1].y_min > regions.navigationBox.y_max);
    options.sections.selected = 1;
    options.sections.expanded = {false, true};
    auto const folded = render();
    CHECK(folded.text.contains("Title — Artist"));
    CHECK(folded.text.contains("Audio Properties"));
    CHECK(folded.text.contains("44100 Hz"));
    CHECK_FALSE(folded.text.contains("xxxx"));
  }
} // namespace ao::tui::test
