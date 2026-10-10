// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "RenderTestSupport.h"
#include "TrackPropertiesEditorTestSupport.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "tui/TrackCreditsEditor.h"
#include "tui/TrackPropertiesEditor.h"
#include <ao/library/Credits.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <array>
#include <cstddef>
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
    TrackPropertiesEditor::CompletionProvider numberedCandidates()
    {
      return [](rt::TrackField, std::string_view const text, std::size_t) -> std::optional<rt::CompletionResult>
      {
        auto items = std::vector<rt::CompletionItem>{};

        for (std::size_t index = 0; index < 15; ++index)
        {
          auto value = std::format("Cand{:02}", index);
          items.push_back(rt::CompletionItem{.displayText = value, .insertText = value});
        }

        return rt::CompletionResult{.replaceBegin = 0, .replaceEnd = text.size(), .items = std::move(items)};
      };
    }
    TrackPropertiesEditor makeCreditsEditor(TrackPropertiesEditor::CompletionProvider completionProvider,
                                            TrackCreditsEditor::CompletionProvider creditCompletionProvider)
    {
      auto storage = rt::test::MusicLibraryFixture{};
      auto changes = rt::test::makeStateOnlyLibraryChanges(storage.library());
      auto commands = rt::test::LibraryCommandsFixture{storage.library(), changes};
      auto const trackId = commands.addTrack({.title = "Track",
                                              .album = "Blue",
                                              .credits = {{"Seed", library::CreditKind::Soloist, "violin"},
                                                          {"Hidden", library::CreditKind::Soloist, "cello"}},
                                              .uri = "/music/track.flac"});
      auto const& textCatalog = ao::test::englishMessageCatalog();
      auto form = uimodel::TrackPropertiesFormModel{textCatalog};
      REQUIRE(uimodel::loadTrackPropertiesFormBaseline(
        commands.library().snapshot(), std::array{trackId}, uimodel::buildTrackPropertiesFormSpec(textCatalog), form));
      return TrackPropertiesEditor{
        textCatalog,
        TrackEditorPreparation{
          .targets = {{.id = trackId, .title = "Track", .path = "/music/track.flac"}}, .baseline = std::move(form)},
        std::move(completionProvider),
        TrackEditorMode::Properties,
        std::move(creditCompletionProvider)};
    }
  } // namespace

  TEST_CASE("TrackPropertiesEditor - completes metadata values from provider", "[tui][unit][editor]")
  {
    bool completionInvoked = false;
    auto const mockCompleter = TrackPropertiesEditor::CompletionProvider{
      [&](rt::TrackField const field,
          std::string_view const text,
          [[maybe_unused]] std::size_t const cursor) -> std::optional<rt::CompletionResult>
      {
        if (field == rt::TrackField::Album)
        {
          completionInvoked = true;
          return rt::CompletionResult{
            .replaceBegin = 0,
            .replaceEnd = text.size(),
            .items =
              {
                rt::CompletionItem{.displayText = "Kind of Blue", .insertText = "Kind of Blue"},
                rt::CompletionItem{.displayText = "Kind of Red", .insertText = "Kind of Red"},
              },
          };
        }

        return std::nullopt;
      }};

    auto editor = makeEditor({TrackFixture{.title = "So What", .album = "", .year = 1959}}, mockCompleter);

    SECTION("Typing into supported field opens completion popup")
    {
      focusRow(editor, "Album");
      typeText(editor, "K");

      CHECK(completionInvoked);
      auto const rendered = frame(editor);
      CHECK(rendered.contains("Kind of Blue"));
      CHECK(rendered.contains("Kind of Red"));

      // Arrow down moves candidate selection
      editor.tryHandleEvent(ftxui::Event::ArrowDown);

      // Enter accepts candidate
      editor.tryHandleEvent(ftxui::Event::Return);
      CHECK(editor.isDirty());
      CHECK(frame(editor).contains("Kind of Red"));

      auto const patch = editor.buildPatch();
      REQUIRE(patch.metadata.optAlbum);
      CHECK(*patch.metadata.optAlbum == "Kind of Red");
    }

    SECTION("Escape dismisses completion without closing editor")
    {
      focusRow(editor, "Album");
      typeText(editor, "K");
      CHECK(frame(editor).contains("Kind of Blue"));

      editor.tryHandleEvent(ftxui::Event::Escape);
      CHECK(editor.takeRequest() == TrackEditorRequest::None);
      CHECK_FALSE(frame(editor).contains("Kind of Blue"));
      CHECK(editor.isDirty());
      CHECK(editor.buildPatch().metadata.optAlbum == "K");
    }

    SECTION("Explicit trigger with Ctrl-N requests completion on empty input")
    {
      focusRow(editor, "Album");
      editor.tryHandleEvent(completeEvent());

      CHECK(completionInvoked);
      CHECK(frame(editor).contains("Kind of Blue"));
    }

    SECTION("Tab closes completion and switches tab without accepting")
    {
      focusRow(editor, "Album");
      typeText(editor, "K");
      CHECK(frame(editor).contains("Kind of Blue"));

      editor.tryHandleEvent(ftxui::Event::Tab);
      CHECK(editor.tab() == TrackEditorTab::Tags);
      CHECK(editor.isDirty());
      CHECK(editor.buildPatch().metadata.optAlbum == "K");
    }
  }

  TEST_CASE("TrackPropertiesEditor - a confirmation prompt takes the surface from the completion popup",
            "[tui][unit][editor]")
  {
    auto const mockCompleter = TrackPropertiesEditor::CompletionProvider{
      [](rt::TrackField const field,
         std::string_view const text,
         [[maybe_unused]] std::size_t const cursor) -> std::optional<rt::CompletionResult>
      {
        if (field != rt::TrackField::Album)
        {
          return std::nullopt;
        }

        return rt::CompletionResult{
          .replaceBegin = 0,
          .replaceEnd = text.size(),
          .items = {rt::CompletionItem{.displayText = "Kind of Blue", .insertText = "Kind of Blue"}},
        };
      }};

    auto editor = makeEditor({TrackFixture{.title = "So What", .album = ""}}, mockCompleter);
    focusRow(editor, "Album");
    typeText(editor, "K");
    REQUIRE(frame(editor).contains("Kind of Blue"));

    SECTION("Ctrl-R closes it on the way past")
    {
      editor.tryHandleEvent(reloadEvent());

      // Every other chord the popup declines closes it first, so a reload
      // prompt is never drawn under candidates it cannot take input for.
      auto const text = frame(editor);
      REQUIRE(editor.isConfirmingReload());
      CHECK_FALSE(text.contains("Kind of Blue"));
    }

    SECTION("The footer names Escape for what it closes")
    {
      // Escape dismisses the popup here and leaves the editor open, so
      // promising "close" would name the wrong exit.
      auto const text = frame(editor);
      CHECK(text.contains("dismiss"));
      CHECK_FALSE(text.contains("close"));
    }
  }

  TEST_CASE("TrackPropertiesEditor - pages and browses candidates without editing", "[tui][unit][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "So What", .album = ""}}, numberedCandidates());
    focusRow(editor, "Album");
    editor.tryHandleEvent(completeEvent());

    SECTION("PageDown moves by the visible page, not by one")
    {
      auto const first = frame(editor);
      CHECK(first.contains("Cand00"));
      CHECK_FALSE(first.contains("Cand06"));

      editor.tryHandleEvent(ftxui::Event::PageDown);

      auto const second = frame(editor);
      CHECK(second.contains("Cand06"));
      CHECK_FALSE(second.contains("Cand00"));

      editor.tryHandleEvent(ftxui::Event::PageUp);
      CHECK(frame(editor).contains("Cand00"));
    }

    SECTION("browsing candidates is not an edit")
    {
      editor.tryHandleEvent(ftxui::Event::ArrowDown);
      editor.tryHandleEvent(ftxui::Event::ArrowDown);
      editor.tryHandleEvent(ftxui::Event::PageDown);

      CHECK_FALSE(editor.isDirty());
      CHECK_FALSE(editor.buildPatch().metadata.optAlbum);
    }

    SECTION("a dismissed candidate cannot reach the row the caret moved to")
    {
      editor.tryHandleEvent(ftxui::Event::ArrowDown); // select Cand01
      editor.tryHandleEvent(ftxui::Event::Escape);    // closes the popup, not the editor
      editor.tryHandleEvent(ftxui::Event::ArrowDown); // move to the next row
      editor.tryHandleEvent(ftxui::Event::Return);    // no candidate is live any more

      CHECK(editor.takeRequest() == TrackEditorRequest::None);
      CHECK_FALSE(editor.isDirty());
      CHECK_FALSE(frame(editor).contains("Cand01"));
    }
  }

  TEST_CASE("TrackPropertiesEditor - word and line caret moves dismiss stale candidates", "[tui][unit][editor]")
  {
    for (auto const& event : {ftxui::Event::CtrlA, ftxui::Event::Special("\033b"), ftxui::Event::ArrowLeftCtrl})
    {
      auto editor = makeEditor({TrackFixture{.title = "Track", .album = "Blue"}}, numberedCandidates());
      focusRow(editor, "Album");
      editor.tryHandleEvent(completeEvent());
      REQUIRE(frame(editor).contains("Cand00"));
      editor.tryHandleEvent(event);
      CHECK_FALSE(frame(editor).contains("Cand00"));
      CHECK_FALSE(editor.isDirty());
    }
  }

  TEST_CASE("TrackPropertiesEditor - caret commands dismiss completion even at a boundary", "[tui][unit][editor]")
  {
    for (auto const& event : {ftxui::Event::CtrlA,
                              ftxui::Event::CtrlE,
                              ftxui::Event::Special("\033b"),
                              ftxui::Event::Special("\033f"),
                              ftxui::Event::ArrowLeftCtrl,
                              ftxui::Event::ArrowRightCtrl})
    {
      auto editor = makeEditor({{"Track", "Blue"}}, numberedCandidates());
      focusRow(editor, "Album");
      REQUIRE(editor.tryHandleEvent(event));
      REQUIRE(editor.tryHandleEvent(completeEvent()));
      REQUIRE(frame(editor).contains("Cand00"));
      REQUIRE(editor.tryHandleEvent(event));
      CHECK_FALSE(frame(editor).contains("Cand00"));
      CHECK_FALSE(editor.isDirty());
    }
  }

  TEST_CASE("TrackPropertiesEditor - scrolls short terminals to the selected completion", "[tui][unit][editor]")
  {
    for (auto const height : {24, 20, 16})
    {
      for (auto const* const label : {"Composer", "Work"})
      {
        DYNAMIC_SECTION(label << " at 80x" << height)
        {
          auto editor = makeEditor({TrackFixture{.title = "Track", .album = "Blue"}}, numberedCandidates());
          focusRow(editor, label);
          editor.tryHandleEvent(completeEvent());

          for (std::size_t step = 0; step < 5; ++step)
          {
            editor.tryHandleEvent(ftxui::Event::ArrowDown);
          }

          auto const rendered = renderElement(editor.renderModal(80, height), 80, height);
          CHECK(rendered.text.contains("> Cand05"));
          CHECK_FALSE(editor.isDirty());
        }
      }
    }
  }

  TEST_CASE("TrackPropertiesEditor - locked Soloist completion stays visible without scalar editing",
            "[tui][unit][editor][credits]")
  {
    for (auto const columns : {80, 48, 32})
    {
      for (auto const height : {24, 20, 16})
      {
        DYNAMIC_SECTION("Soloist at " << columns << "x" << height)
        {
          bool scalarCompletionInvoked = false;
          bool creditCompletionInvoked = false;
          auto editor = makeCreditsEditor(
            [&](rt::TrackField, std::string_view, std::size_t) -> std::optional<rt::CompletionResult>
            {
              scalarCompletionInvoked = true;
              return std::nullopt;
            },
            [&](library::CreditKind const kind, bool const role, std::string_view const text, std::size_t const cursor)
            {
              creditCompletionInvoked = true;
              CHECK(kind == library::CreditKind::Soloist);
              CHECK_FALSE(role);
              CHECK(text == "Seed");
              CHECK(cursor == 4);
              return numberedCandidates()(rt::TrackField::Soloist, text, cursor);
            });
          focusRow(editor, "Soloist");
          editor.tryHandleEvent(completeEvent());
          typeText(editor, "not a scalar");
          CHECK_FALSE(scalarCompletionInvoked);
          CHECK_FALSE(editor.isDirty());
          REQUIRE(editor.tryHandleEvent(ftxui::Event::Return));
          REQUIRE(editor.isEditingCredits());

          editor.tryHandleEvent(completeEvent());
          REQUIRE(creditCompletionInvoked);

          for (std::size_t step = 0; step < 5; ++step)
          {
            editor.tryHandleEvent(ftxui::Event::ArrowDown);
          }

          auto rendered = renderElement(editor.renderModal(columns, height), columns, height);
          INFO(rendered.text);
          CHECK(rendered.text.contains("> Cand05"));
          CHECK_FALSE(rendered.text.contains("Cand06"));
          CHECK_FALSE(editor.canApply());
          CHECK_FALSE(editor.buildPatch().metadata.optCredits);
          CHECK(editor.takeRequest() == TrackEditorRequest::None);

          auto const optTab = findTextCells(rendered.screen, "Tags");
          REQUIRE(optTab);
          REQUIRE(editor.tryHandleEvent(ftxui::Event::Mouse(
            "",
            ftxui::Mouse{
              .button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = optTab->x_min, .y = optTab->y_min})));
          CHECK(editor.tab() == TrackEditorTab::Metadata);
          CHECK(editor.isEditingCredits());
          rendered = renderElement(editor.renderModal(columns, height), columns, height);
          CHECK(rendered.text.contains("> Cand05"));

          SECTION("accepting a candidate preserves the locked kind and role in the parent draft")
          {
            auto candidate = std::string{"Cand05"};

            SECTION("Enter accepts the keyboard selection")
            {
              editor.tryHandleEvent(ftxui::Event::Return);
            }

            SECTION("clicking accepts the painted candidate instead of the keyboard selection")
            {
              candidate = "Cand04";
              auto const optCandidate = findTextCells(rendered.screen, candidate);
              REQUIRE(optCandidate);
              REQUIRE(editor.tryHandleEvent(ftxui::Event::Mouse("",
                                                                ftxui::Mouse{.button = ftxui::Mouse::Left,
                                                                             .motion = ftxui::Mouse::Pressed,
                                                                             .x = optCandidate->x_min,
                                                                             .y = optCandidate->y_min})));
            }

            CHECK(editor.isEditingCredits());
            editor.tryHandleEvent(applyEvent());
            CHECK_FALSE(editor.isEditingCredits());
            CHECK(editor.takeRequest() == TrackEditorRequest::None);
            auto const patch = editor.buildPatch();
            REQUIRE(patch.metadata.optCredits);
            CHECK(patch.metadata.optCredits->kinds == uimodel::trackCreditScope(library::CreditKind::Soloist));
            CHECK(patch.metadata.optCredits->entries ==
                  std::vector<library::Credit>{{candidate, library::CreditKind::Soloist, "violin"},
                                               {"Hidden", library::CreditKind::Soloist, "cello"}});
            CHECK_FALSE(patch.metadata.optTitle);
            CHECK_FALSE(patch.metadata.optAlbum);
          }

          SECTION("dismissing completion then cancelling the child leaves no edit")
          {
            editor.tryHandleEvent(ftxui::Event::Escape);
            CHECK(editor.isEditingCredits());
            CHECK_FALSE(renderElement(editor.renderModal(columns, height), columns, height).text.contains("Cand05"));
            editor.tryHandleEvent(ftxui::Event::Escape);
            CHECK_FALSE(editor.isEditingCredits());
            CHECK_FALSE(editor.isDirty());
            CHECK_FALSE(editor.buildPatch().metadata.optCredits);
            CHECK(editor.takeRequest() == TrackEditorRequest::None);
          }
        }
      }
    }
  }

  TEST_CASE("TrackPropertiesEditor - reversing candidate navigation preserves the visible window",
            "[tui][unit][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "Track", .album = ""}}, numberedCandidates());
    focusRow(editor, "Album");
    editor.tryHandleEvent(completeEvent());

    for (std::size_t step = 0; step < 6; ++step)
    {
      editor.tryHandleEvent(ftxui::Event::ArrowDown);
    }

    REQUIRE(frame(editor).contains("> Cand06"));
    editor.tryHandleEvent(ftxui::Event::ArrowUp);

    auto const rendered = frame(editor);
    CHECK(rendered.contains("> Cand05"));
    CHECK(rendered.contains("Cand01"));
    CHECK(rendered.contains("Cand06"));
    CHECK_FALSE(rendered.contains("Cand00"));
  }

  TEST_CASE("TrackPropertiesEditor - page navigation advances a full candidate window", "[tui][unit][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "Track", .album = ""}}, numberedCandidates());
    focusRow(editor, "Album");
    editor.tryHandleEvent(completeEvent());
    editor.tryHandleEvent(ftxui::Event::PageDown);

    auto const rendered = frame(editor);
    CHECK(rendered.contains("> Cand06"));
    CHECK(rendered.contains("Cand11"));
    CHECK_FALSE(rendered.contains("Cand05"));

    editor.tryHandleEvent(ftxui::Event::Return);
    CHECK(editor.buildPatch().metadata.optAlbum == "Cand06");
  }
} // namespace ao::tui::test
