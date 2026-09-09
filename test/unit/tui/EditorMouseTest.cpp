// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/tui/RenderTestSupport.h"
#include "test/unit/tui/TrackPropertiesEditorTestSupport.h"
#include "tui/TrackPropertiesEditor.h"
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/completion/CompletionResult.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    ftxui::Event clickText(ftxui::Screen const& screen, std::string_view const label)
    {
      auto const optBox = findTextCells(screen, label);
      REQUIRE(optBox);
      return ftxui::Event::Mouse(
        "",
        ftxui::Mouse{
          .button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = optBox->x_min, .y = optBox->y_min});
    }
  } // namespace

  TEST_CASE("TrackPropertiesEditor - mouse selects pages and fields without changing the draft",
            "[tui][unit][mouse][editor]")
  {
    auto editor = makeEditor({{"Blue", "Album"}});
    auto rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "Album Artist")));
    CHECK_FALSE(editor.isDirty());
    REQUIRE(editor.tryHandleEvent(ftxui::Event::Character("Mouse")));
    auto patch = editor.buildPatch();
    REQUIRE(patch.metadata.optAlbumArtist);
    CHECK(*patch.metadata.optAlbumArtist == "Mouse");
    CHECK_FALSE(patch.metadata.optTitle);

    rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "Tags")));
    CHECK(editor.tab() == TrackEditorTab::Tags);
    rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "Metadata")));
    CHECK(editor.tab() == TrackEditorTab::Metadata);
    CHECK(editor.buildPatch().metadata.optAlbumArtist == patch.metadata.optAlbumArtist);
  }

  TEST_CASE("TrackPropertiesEditor - pointer motion and release preserve the next click",
            "[tui][regression][mouse][editor]")
  {
    auto editor = makeEditor({{"Blue", "Album"}});
    auto const rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    auto click = clickText(rendered.screen, "Tags");
    auto mouse = click.mouse();
    mouse.motion = ftxui::Mouse::Moved;
    REQUIRE(editor.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    mouse.motion = ftxui::Mouse::Released;
    REQUIRE(editor.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    REQUIRE(editor.tryHandleEvent(click));
    CHECK(editor.tab() == TrackEditorTab::Tags);
    CHECK_FALSE(editor.isDirty());
  }

  TEST_CASE("TrackPropertiesEditor - clicking the active tab preserves its query", "[tui][regression][mouse][editor]")
  {
    auto editor = makeEditor({{"Blue", "Album"}}, {}, {}, {"Jazz", "Rock"});
    selectTab(editor, TrackEditorTab::Tags);
    typeText(editor, "Rock");
    auto const rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "Tags")));
    REQUIRE(editor.tryHandleEvent(ftxui::Event::Return));
    CHECK(editor.buildPatch().tagsToAdd == std::vector<std::string>{"Rock"});
  }

  TEST_CASE("TrackPropertiesEditor - wheel navigation survives pointer motion without changing tag intent",
            "[tui][regression][mouse][editor]")
  {
    auto editor = makeEditor({{"Blue", "Album"}}, {}, {{"Jazz", 1}}, {"Rock"});
    selectTab(editor, TrackEditorTab::Tags);
    auto const rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    auto click = clickText(rendered.screen, "Jazz");
    auto mouse = click.mouse();
    mouse.motion = ftxui::Mouse::Moved;
    REQUIRE(editor.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    mouse.button = ftxui::Mouse::WheelDown;
    mouse.motion = ftxui::Mouse::Pressed;
    REQUIRE(editor.tryHandleEvent(ftxui::Event::Mouse("", mouse)));
    CHECK_FALSE(editor.isDirty());
    REQUIRE(editor.tryHandleEvent(ftxui::Event::Return));
    CHECK(editor.buildPatch().tagsToAdd == std::vector<std::string>{"Rock"});
    CHECK(editor.buildPatch().tagsToRemove.empty());
  }

  TEST_CASE("TrackPropertiesEditor - tag clicks share intent and Apply protocol", "[tui][unit][mouse][editor]")
  {
    auto editor = makeEditor({{"Blue", "Album"}}, {}, {{"Jazz", 1}}, {"Rock"});
    selectTab(editor, TrackEditorTab::Tags);
    auto rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "Rock")));
    CHECK(editor.buildPatch().tagsToAdd == std::vector<std::string>{"Rock"});
    rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "Jazz")));
    CHECK(editor.buildPatch().tagsToRemove == std::vector<std::string>{"Jazz"});
    rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "Ctrl-S")));
    CHECK(editor.takeRequest() == TrackEditorRequest::Apply);
  }

  TEST_CASE("TrackPropertiesEditor - clickable discard prompt preserves cancellation",
            "[tui][regression][mouse][editor]")
  {
    auto editor = makeEditor({{"Blue", "Album"}});
    typeText(editor, "x");
    auto rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "Esc")));
    CHECK(editor.isConfirmingDiscard());
    rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "[Esc]")));
    CHECK_FALSE(editor.isConfirmingDiscard());
    CHECK(editor.isDirty());
    CHECK(editor.takeRequest() == TrackEditorRequest::None);
    rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "Esc")));
    rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "[Enter]")));
    CHECK(editor.takeRequest() == TrackEditorRequest::Close);
  }

  TEST_CASE("TrackPropertiesEditor - keyboard page changes invalidate old mouse regions",
            "[tui][regression][mouse][editor]")
  {
    auto editor = makeEditor({{"Blue", "Album"}});
    auto const rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    auto const oldClick = clickText(rendered.screen, "Properties");
    REQUIRE(editor.tryHandleEvent(ftxui::Event::Tab));
    REQUIRE(editor.tryHandleEvent(oldClick));
    CHECK(editor.tab() == TrackEditorTab::Tags);
    CHECK_FALSE(editor.isDirty());
  }

  TEST_CASE("TrackPropertiesEditor - clicking an inline completion accepts that candidate",
            "[tui][regression][mouse][editor]")
  {
    auto editor = makeEditor(
      {{"Blue", ""}},
      [](rt::TrackField, std::string_view text, std::size_t) -> std::optional<rt::CompletionResult>
      {
        return rt::CompletionResult{.replaceBegin = 0,
                                    .replaceEnd = text.size(),
                                    .items = {{.displayText = "First album", .insertText = "First album"},
                                              {.displayText = "Second album", .insertText = "Second album"}}};
      });
    focusRow(editor, "Album");
    typeText(editor, "a");
    auto const rendered = renderElement(editor.renderModal(48, 18), 48, 18);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "Second album")));
    CHECK(editor.buildPatch().metadata.optAlbum == "Second album");
    CHECK_FALSE(frame(editor).contains("First album"));
    CHECK(editor.takeRequest() == TrackEditorRequest::None);
  }

  TEST_CASE("TrackPropertiesEditor - pointer placement follows a horizontally scrolled field",
            "[tui][regression][mouse][editor]")
  {
    auto const prefix = std::string(80, 'x');
    auto editor = makeEditor({{prefix + "pointer", "Album"}});
    auto const rendered = renderElement(editor.renderModal(48, 18), 48, 18);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "pointer")));
    CHECK_FALSE(editor.isDirty());
    typeText(editor, "_");
    CHECK(editor.buildPatch().metadata.optTitle == prefix + "_pointer");
  }

  TEST_CASE("TrackPropertiesEditor - tag query pointer placement follows the rendered text origin",
            "[tui][regression][mouse][editor]")
  {
    for (auto const& prefix : {std::string{"place "}, std::string(80, 'x')})
    {
      auto editor = makeEditor({{"Blue", "Album"}});
      selectTab(editor, TrackEditorTab::Tags);
      typeText(editor, prefix + "pointer");
      auto const rendered = renderElement(editor.renderModal(48, 18), 48, 18);
      REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "pointer")));
      typeText(editor, "_");
      CHECK_FALSE(editor.isDirty());
      REQUIRE(editor.tryHandleEvent(ftxui::Event::Return));
      CHECK(editor.buildPatch().tagsToAdd == std::vector<std::string>{prefix + "_pointer"});
    }
  }

  TEST_CASE("TrackPropertiesEditor - a submitting write ignores a previously painted close button",
            "[tui][regression][mouse][editor]")
  {
    auto editor = makeEditor({{"Blue", "Album"}});
    auto const rendered = renderElement(editor.renderModal(48, 18), 48, 18);
    editor.setStatus(TrackEditorStatus::Submitting);
    REQUIRE(editor.tryHandleEvent(clickText(rendered.screen, "×")));
    CHECK(editor.status() == TrackEditorStatus::Submitting);
    CHECK(editor.takeRequest() == TrackEditorRequest::None);
  }
} // namespace ao::tui::test
