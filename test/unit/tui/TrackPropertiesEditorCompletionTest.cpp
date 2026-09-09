// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "RenderTestSupport.h"
#include "TrackPropertiesEditorTestSupport.h"
#include "tui/TrackPropertiesEditor.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>

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

  TEST_CASE("TrackPropertiesEditor - word and line caret moves dismiss stale candidates", "[tui][regression][editor]")
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

  TEST_CASE("TrackPropertiesEditor - caret commands dismiss completion even at a boundary", "[tui][regression][editor]")
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

  TEST_CASE("TrackPropertiesEditor - scrolls short terminals to the selected completion", "[tui][regression][editor]")
  {
    for (auto const height : {24, 20, 16})
    {
      for (auto const* const label : {"Composer", "Work", "Soloist"})
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

  TEST_CASE("TrackPropertiesEditor - reversing candidate navigation preserves the visible window",
            "[tui][regression][editor]")
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

  TEST_CASE("TrackPropertiesEditor - page navigation advances a full candidate window", "[tui][regression][editor]")
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
