// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "RenderTestSupport.h"
#include "TrackPropertiesEditorTestSupport.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "tui/TrackPropertiesEditor.h"
#include <ao/i18n/MessageCatalog.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  TEST_CASE("TrackTagPopover - applies only explicit tag changes to the captured batch", "[tui][unit][editor]")
  {
    auto editor = makeEditor({{.title = "First", .album = "Album"}, {.title = "Second", .album = "Album"}},
                             {},
                             {{"Jazz", 2}, {"Modal", 1}},
                             {},
                             "en",
                             TrackEditorMode::Tags);
    auto const initial = renderElement(editor.renderModal(80, 24), 80, 24).text;
    CHECK(initial.contains("Tags · 2 tracks"));
    CHECK_FALSE(initial.contains("Metadata"));
    CHECK_FALSE(initial.contains("First"));
    CHECK(initial.contains("1/2"));

    editor.tryHandleEvent(ftxui::Event::ArrowDown);
    editor.tryHandleEvent(ftxui::Event::Character(' '));
    CHECK(editor.takeRequest() == TrackEditorRequest::None);
    CHECK(editor.buildPatch().tagsToAdd == std::vector<std::string>{"Modal"});
    CHECK(editor.patchSummary().fieldCount == 0);
    CHECK(editor.patchSummary().clearCount == 0);
    editor.tryHandleEvent(ftxui::Event::Return);
    CHECK(editor.takeRequest() == TrackEditorRequest::Apply);
  }

  TEST_CASE("TrackTagPopover - accepts multiword new names and cancels drafts directly", "[tui][unit][editor]")
  {
    auto editor =
      makeEditor({{.title = "First", .album = "Album"}}, {}, {{"Jazz", 1}}, {}, "en", TrackEditorMode::Tags);
    editor.tryHandleEvent(ftxui::Event::Character(' '));
    typeText(editor, "late night");
    CHECK(frame(editor).contains("late night"));

    SECTION("Enter creates the tag and applies both changes")
    {
      editor.tryHandleEvent(ftxui::Event::Return);
      CHECK(editor.takeRequest() == TrackEditorRequest::Apply);
      auto const patch = editor.buildPatch();
      CHECK(patch.tagsToAdd == std::vector<std::string>{"late night"});
      CHECK(patch.tagsToRemove == std::vector<std::string>{"Jazz"});
      CHECK(editor.patchSummary().fieldCount == 0);
      CHECK(editor.patchSummary().clearCount == 0);
    }

    SECTION("Escape discards a dirty draft and its query with one press")
    {
      editor.tryHandleEvent(ftxui::Event::Escape);
      CHECK(editor.takeRequest() == TrackEditorRequest::Close);
      CHECK_FALSE(editor.isConfirmingDiscard());
    }

    SECTION("Submitting consumes close and edit keys")
    {
      editor.setStatus(TrackEditorStatus::Submitting);
      editor.tryHandleEvent(ftxui::Event::Escape);
      editor.tryHandleEvent(ftxui::Event::Return);
      editor.tryHandleEvent(ftxui::Event::CtrlR);
      CHECK(editor.takeRequest() == TrackEditorRequest::None);
      CHECK(editor.buildPatch().tagsToAdd.empty());
    }
  }

  TEST_CASE("TrackTagPopover - query matches do not become implicit edits on apply", "[tui][unit][editor]")
  {
    auto editor = makeEditor({{.title = "First", .album = "Album"}}, {}, {}, {"Acoustic"}, "en", TrackEditorMode::Tags);
    typeText(editor, "Acoustic");
    editor.tryHandleEvent(ftxui::Event::Return);
    CHECK(editor.takeRequest() == TrackEditorRequest::Close);
    CHECK_FALSE(editor.isDirty());
  }

  TEST_CASE("TrackTagPopover - mouse motion preserves the rendered row click target", "[tui][unit][editor]")
  {
    auto editor =
      makeEditor({{.title = "First", .album = "Album"}}, {}, {{"Jazz", 1}}, {}, "en", TrackEditorMode::Tags);
    auto const rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    auto const optBox = findTextCells(rendered.screen, "Jazz");
    REQUIRE(optBox);
    auto mouse =
      ftxui::Mouse{.button = ftxui::Mouse::None, .motion = ftxui::Mouse::Moved, .x = optBox->x_min, .y = optBox->y_min};
    // Motion must not consume the readiness of the last rendered hit regions.
    editor.tryHandleEvent(ftxui::Event::Mouse("", mouse));
    mouse.button = ftxui::Mouse::Left;
    mouse.motion = ftxui::Mouse::Pressed;
    editor.tryHandleEvent(ftxui::Event::Mouse("", mouse));
    CHECK(editor.buildPatch().tagsToRemove == std::vector<std::string>{"Jazz"});
  }

  TEST_CASE("TrackTagPopover - narrow rows keep spacing after pending status", "[tui][unit][editor][layout]")
  {
    auto editor =
      makeEditor({{.title = "First", .album = "Album"}}, {}, {{"Jazz", 1}}, {}, "en", TrackEditorMode::Tags);
    editor.tryHandleEvent(ftxui::Event::Character(' '));
    auto const narrow = renderElement(editor.renderModal(48, 18), 48, 18);
    auto const optStatusBox = findTextCells(narrow.screen, "Remove");
    REQUIRE(optStatusBox);
    CHECK(narrow.screen.PixelAt(optStatusBox->x_max + 1, optStatusBox->y_min).character == " ");
  }

  TEST_CASE("TrackTagPopover - outside press cancels a dirty draft", "[tui][unit][editor]")
  {
    auto editor =
      makeEditor({{.title = "First", .album = "Album"}}, {}, {{"Jazz", 1}}, {}, "en", TrackEditorMode::Tags);
    editor.tryHandleEvent(ftxui::Event::Character(' '));
    renderElement(editor.renderModal(80, 24), 80, 24);
    auto const mouse = ftxui::Mouse{.button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = 0, .y = 0};
    editor.tryHandleEvent(ftxui::Event::Mouse("", mouse));
    CHECK(editor.takeRequest() == TrackEditorRequest::Close);
    CHECK_FALSE(editor.isConfirmingDiscard());
  }

  TEST_CASE("TrackTagPopover - exit label distinguishes a clean view from pending tag changes",
            "[tui][regression][editor]")
  {
    constexpr auto kLabels = std::array{std::tuple{"en", "Close", "Discard changes"},
                                        std::tuple{"zh-Hans", "关闭", "放弃更改"},
                                        std::tuple{"zh-Hant", "關閉", "捨棄變更"},
                                        std::tuple{"ja", "閉じる", "変更を破棄"},
                                        std::tuple{"de", "Schließen", "Änderungen verwerfen"},
                                        std::tuple{"es", "Cerrar", "Descartar cambios"},
                                        std::tuple{"fr", "Fermer", "Abandonner les modifications"}};

    for (auto const& [locale, close, discard] : kLabels)
    {
      CAPTURE(locale);
      auto editor =
        makeEditor({{.title = "First", .album = "Album"}}, {}, {{"Jazz", 1}}, {}, locale, TrackEditorMode::Tags);
      auto const clean = frame(editor);
      CHECK(clean.contains(close));
      CHECK_FALSE(clean.contains(discard));

      editor.tryHandleEvent(ftxui::Event::Character(' '));
      REQUIRE(editor.isDirty());
      auto const dirty = frame(editor);
      CHECK(dirty.contains(discard));
      CHECK_FALSE(dirty.contains(close));

      editor.tryHandleEvent(ftxui::Event::Character(' '));
      REQUIRE_FALSE(editor.isDirty());
      auto const restored = frame(editor);
      CHECK(restored.contains(close));
      CHECK_FALSE(restored.contains(discard));

      typeText(editor, "New");
      CHECK_FALSE(editor.isDirty());
      auto const offered = frame(editor);
      CHECK(offered.contains(discard));
      CHECK_FALSE(offered.contains(close));

      editor.tryHandleEvent(ftxui::Event::Escape);
      CHECK(editor.takeRequest() == TrackEditorRequest::Close);
      CHECK_FALSE(editor.isConfirmingDiscard());
    }
  }

  TEST_CASE("TrackTagPopover - clearing a new-tag query and filtering an existing tag keep the view clean",
            "[tui][regression][editor]")
  {
    auto editor =
      makeEditor({{.title = "First", .album = "Album"}}, {}, {{"Jazz", 1}}, {}, "en", TrackEditorMode::Tags);
    typeText(editor, "X");
    CHECK(frame(editor).contains("Discard changes"));
    editor.tryHandleEvent(ftxui::Event::Backspace);
    auto const cleared = frame(editor);
    CHECK(cleared.contains("Close"));
    CHECK_FALSE(cleared.contains("Discard changes"));
    CHECK_FALSE(editor.isDirty());

    typeText(editor, "Jazz");
    auto const filtered = frame(editor);
    CHECK(filtered.contains("Close"));
    CHECK_FALSE(filtered.contains("Discard changes"));
    CHECK_FALSE(editor.isDirty());
    editor.tryHandleEvent(ftxui::Event::Escape);
    CHECK(editor.takeRequest() == TrackEditorRequest::Close);
    CHECK_FALSE(editor.isConfirmingDiscard());
  }

  TEST_CASE("TrackTagPopover - localized narrow frames retain clickable clean and draft exit controls",
            "[tui][unit][editor][layout]")
  {
    for (auto const* locale : {"en", "zh-Hans", "zh-Hant", "ja", "de", "es", "fr"})
    {
      auto const catalog = ao::test::messageCatalog(locale);

      for (bool const offersTag : {false, true})
      {
        auto const applyLabel =
          offersTag ? i18n::MessageId::TuiTagPopoverAddApply : i18n::MessageId::TuiTagPopoverApply;
        auto const exitLabel = offersTag ? i18n::MessageId::TuiTagPopoverCancel : i18n::MessageId::TuiTagPopoverClose;

        for (std::int32_t const width : {24, 36, 48, 80})
        {
          for (bool const clickLabel : {false, true})
          {
            CAPTURE(locale, offersTag, width, clickLabel);
            auto editor =
              makeEditor({{.title = "First", .album = "Album"}}, {}, {{"Jazz", 1}}, {}, locale, TrackEditorMode::Tags);

            if (offersTag)
            {
              typeText(editor, "New");
            }

            auto const rendered = renderElement(editor.renderModal(width, 16), width, 16);
            INFO(rendered.text);
            CHECK(rendered.text.contains("Enter"));
            auto const optExitKeyBox = findTextCells(rendered.screen, "Esc");
            REQUIRE(optExitKeyBox);

            if (width == 80)
            {
              auto const optApplyKeyBox = findTextCells(rendered.screen, "Enter");
              REQUIRE(optApplyKeyBox);
              CHECK(optApplyKeyBox->y_min == optExitKeyBox->y_min);
            }

            if (width >= 48)
            {
              CHECK(rendered.text.contains(i18n::requiredText(catalog, applyLabel)));
            }

            if (width >= 48 || !offersTag)
            {
              CHECK(rendered.text.contains(i18n::requiredText(catalog, exitLabel)));
            }

            // The label starts after the key and its single separating space.
            auto const clickColumn = clickLabel ? optExitKeyBox->x_max + 2 : optExitKeyBox->x_min;
            CHECK(rendered.screen.PixelAt(clickColumn, optExitKeyBox->y_min).character != " ");
            auto const mouse = ftxui::Mouse{.button = ftxui::Mouse::Left,
                                            .motion = ftxui::Mouse::Pressed,
                                            .x = clickColumn,
                                            .y = optExitKeyBox->y_min};
            editor.tryHandleEvent(ftxui::Event::Mouse("", mouse));
            CHECK(editor.takeRequest() == TrackEditorRequest::Close);
            CHECK_FALSE(editor.isConfirmingDiscard());
          }
        }
      }
    }
  }

  TEST_CASE("TrackTagPopover - narrow failure frames preserve recovery controls and drafts",
            "[tui][unit][editor][layout]")
  {
    constexpr auto kVisibleLabels =
      std::array{std::tuple{"de", "Schließen", "Änderungen"}, std::tuple{"fr", "Fermer", "Abandonner"}};

    for (auto const& [locale, close, discardPrefix] : kVisibleLabels)
    {
      for (auto const status : {TrackEditorStatus::Stale, TrackEditorStatus::Failed})
      {
        for (bool const hasEdits : {false, true})
        {
          CAPTURE(locale, status, hasEdits);
          auto editor =
            makeEditor({{.title = "First", .album = "Album"}}, {}, {{"Jazz", 1}}, {}, locale, TrackEditorMode::Tags);

          if (hasEdits)
          {
            editor.tryHandleEvent(ftxui::Event::Character(' '));
          }

          auto const expectedRemovals = hasEdits ? std::vector<std::string>{"Jazz"} : std::vector<std::string>{};
          editor.setStatus(status, std::string(400, 'x'));
          auto const rendered = renderElement(editor.renderModal(24, 16), 24, 16);
          INFO(rendered.text);
          CHECK(rendered.text.contains(hasEdits ? discardPrefix : close));
          CHECK_FALSE(rendered.text.contains(hasEdits ? close : discardPrefix));
          auto const optReloadBox = findTextCells(rendered.screen, "Ctrl-R");
          REQUIRE(optReloadBox);
          auto mouse = ftxui::Mouse{.button = ftxui::Mouse::Left,
                                    .motion = ftxui::Mouse::Pressed,
                                    .x = optReloadBox->x_min,
                                    .y = optReloadBox->y_min};
          editor.tryHandleEvent(ftxui::Event::Mouse("", mouse));
          CHECK(editor.takeRequest() == TrackEditorRequest::Reload);
          CHECK(editor.buildPatch().tagsToRemove == expectedRemovals);

          for (auto const* target : {"Esc", hasEdits ? discardPrefix : close})
          {
            auto const nextFrame = renderElement(editor.renderModal(24, 16), 24, 16);
            auto const optExitBox = findTextCells(nextFrame.screen, target);
            REQUIRE(optExitBox);
            mouse.x = optExitBox->x_max;
            mouse.y = optExitBox->y_min;
            editor.tryHandleEvent(ftxui::Event::Mouse("", mouse));
            CHECK(editor.takeRequest() == TrackEditorRequest::Close);
            CHECK_FALSE(editor.isConfirmingDiscard());
          }
        }
      }
    }
  }

  TEST_CASE("TrackTagPopover - Tab switches Space between query text and tag intent", "[tui][unit][editor]")
  {
    for (auto const& tab : {ftxui::Event::Tab, ftxui::Event::TabReverse})
    {
      auto editor =
        makeEditor({{.title = "First", .album = "Album"}}, {}, {{"Jazz", 1}}, {}, "en", TrackEditorMode::Tags);
      typeText(editor, "Jazz");
      editor.tryHandleEvent(tab);
      editor.tryHandleEvent(ftxui::Event::Character(' '));
      CHECK(editor.buildPatch().tagsToRemove == std::vector<std::string>{"Jazz"});
      editor.tryHandleEvent(ftxui::Event::CtrlG);
      CHECK_FALSE(editor.isDirty());
      editor.tryHandleEvent(tab);
      typeText(editor, "Jazz");
      editor.tryHandleEvent(ftxui::Event::Character(' '));
      typeText(editor, "night");
      editor.tryHandleEvent(ftxui::Event::Return);
      CHECK(editor.buildPatch().tagsToAdd == std::vector<std::string>{"Jazz night"});
      CHECK(editor.buildPatch().tagsToRemove.empty());
    }
  }

  TEST_CASE("TrackTagPopover - deletion from results restores query focus", "[tui][unit][editor]")
  {
    for (auto const& deletion : {ftxui::Event::Delete, ftxui::Event::Backspace})
    {
      auto editor = makeEditor({{.title = "First", .album = "Album"}}, {}, {}, {}, "en", TrackEditorMode::Tags);
      typeText(editor, "newX");

      if (deletion == ftxui::Event::Delete)
      {
        editor.tryHandleEvent(ftxui::Event::ArrowLeft);
      }

      editor.tryHandleEvent(ftxui::Event::ArrowDown);
      editor.tryHandleEvent(deletion);
      editor.tryHandleEvent(ftxui::Event::Character(' '));
      typeText(editor, "tag");
      editor.tryHandleEvent(ftxui::Event::Return);
      CHECK(editor.buildPatch().tagsToAdd == std::vector<std::string>{"new tag"});
    }
  }

  TEST_CASE("TrackTagPopover - page navigation reaches distant results and returns to the start", "[tui][unit][editor]")
  {
    auto tags = std::vector<std::pair<std::string, std::size_t>>{};

    for (char ch = 'A'; ch <= 'Z'; ++ch)
    {
      tags.emplace_back(std::string(1, ch), 1);
    }

    auto editor = makeEditor({{.title = "First", .album = "Album"}}, {}, tags, {}, "en", TrackEditorMode::Tags);

    for (std::int32_t page = 0; page < 30; ++page)
    {
      renderElement(editor.renderModal(48, 16), 48, 16);
      editor.tryHandleEvent(ftxui::Event::PageDown);
    }

    editor.tryHandleEvent(ftxui::Event::ArrowUp);
    editor.tryHandleEvent(ftxui::Event::Character(' '));
    CHECK(editor.buildPatch().tagsToRemove == std::vector<std::string>{"Y"});
    editor.tryHandleEvent(ftxui::Event::CtrlG);

    for (std::int32_t page = 0; page < 30; ++page)
    {
      renderElement(editor.renderModal(48, 16), 48, 16);
      editor.tryHandleEvent(ftxui::Event::PageUp);
    }

    editor.tryHandleEvent(ftxui::Event::Character(' '));
    CHECK(editor.buildPatch().tagsToRemove == std::vector<std::string>{"A"});
  }

  TEST_CASE("TrackTagPopover - wheel moves result focus in both directions", "[tui][unit][editor]")
  {
    auto editor = makeEditor(
      {{.title = "First", .album = "Album"}}, {}, {{"Jazz", 1}, {"Modal", 1}}, {}, "en", TrackEditorMode::Tags);

    for (auto const button : {ftxui::Mouse::WheelDown, ftxui::Mouse::WheelUp})
    {
      auto const rendered = renderElement(editor.renderModal(80, 24), 80, 24);
      auto const optBox = findTextCells(rendered.screen, "Jazz");
      REQUIRE(optBox);
      auto const mouse =
        ftxui::Mouse{.button = button, .motion = ftxui::Mouse::Pressed, .x = optBox->x_min, .y = optBox->y_min};
      editor.tryHandleEvent(ftxui::Event::Mouse("", mouse));
      editor.tryHandleEvent(ftxui::Event::Character(' '));
      CHECK(editor.buildPatch().tagsToRemove ==
            std::vector<std::string>{button == ftxui::Mouse::WheelDown ? "Modal" : "Jazz"});
      editor.tryHandleEvent(ftxui::Event::CtrlG);
    }
  }

  TEST_CASE("TrackTagPopover - single target title identifies the captured track", "[tui][unit][editor][layout]")
  {
    auto editor = makeEditor({{.title = "So What", .album = "Album"}}, {}, {}, {}, "en", TrackEditorMode::Tags);
    auto const rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    CHECK(rendered.text.contains("Tags · 1 track"));
    CHECK(rendered.text.contains("So What"));
  }

  TEST_CASE("TrackTagPopover - short terminals retain clickable cancel and apply", "[tui][unit][editor][layout]")
  {
    for (auto const height : {10, 12})
    {
      for (bool const apply : {false, true})
      {
        auto editor =
          makeEditor({{.title = "First", .album = "Album"}}, {}, {{"Jazz", 1}}, {}, "en", TrackEditorMode::Tags);
        typeText(editor, "New");
        auto const rendered = renderElement(editor.renderModal(24, height), 24, height);
        CAPTURE(height, apply, rendered.text);
        CHECK(rendered.text.contains("New"));
        auto const optAction = findTextCells(rendered.screen, apply ? "Enter" : "Esc");
        REQUIRE(optAction);
        auto const mouse = ftxui::Mouse{
          .button = ftxui::Mouse::Left, .motion = ftxui::Mouse::Pressed, .x = optAction->x_min, .y = optAction->y_min};
        editor.tryHandleEvent(ftxui::Event::Mouse("", mouse));
        CHECK(editor.takeRequest() == (apply ? TrackEditorRequest::Apply : TrackEditorRequest::Close));

        if (apply)
        {
          CHECK(editor.buildPatch().tagsToAdd == std::vector<std::string>{"New"});
        }
      }
    }
  }

  TEST_CASE("TrackTagPopover - query Enter creates an offered name alongside partial matches",
            "[tui][regression][editor]")
  {
    auto editor = makeEditor({{.title = "First", .album = "Album"}}, {}, {}, {"jazzy"}, "en", TrackEditorMode::Tags);
    typeText(editor, "jazz");
    auto const rendered = renderElement(editor.renderModal(64, 20), 64, 20);
    CHECK(rendered.text.contains("jazzy"));
    CHECK(rendered.text.contains("Add new tag"));
    CHECK(rendered.text.contains("Add & apply"));
    editor.tryHandleEvent(ftxui::Event::Return);
    CHECK(editor.takeRequest() == TrackEditorRequest::Apply);
    CHECK(editor.buildPatch().tagsToAdd == std::vector<std::string>{"jazz"});
  }

  TEST_CASE("TrackTagPopover - results activation keeps the explicitly chosen partial match", "[tui][unit][editor]")
  {
    auto editor = makeEditor({{.title = "First", .album = "Album"}}, {}, {}, {"jazzy"}, "en", TrackEditorMode::Tags);
    typeText(editor, "jazz");
    editor.tryHandleEvent(ftxui::Event::Tab);
    editor.tryHandleEvent(ftxui::Event::Character(' '));
    editor.tryHandleEvent(ftxui::Event::Return);
    CHECK(editor.takeRequest() == TrackEditorRequest::Apply);
    CHECK(editor.buildPatch().tagsToAdd == std::vector<std::string>{"jazzy"});
  }

  TEST_CASE("TrackTagPopover - ready reload shortcut preserves the pending draft", "[tui][unit][editor]")
  {
    auto editor =
      makeEditor({{.title = "First", .album = "Album"}}, {}, {{"Jazz", 1}}, {}, "en", TrackEditorMode::Tags);
    editor.tryHandleEvent(ftxui::Event::Character(' '));
    typeText(editor, "new tag");
    editor.tryHandleEvent(ftxui::Event::CtrlR);
    CHECK(editor.takeRequest() == TrackEditorRequest::None);
    CHECK(editor.buildPatch().tagsToRemove == std::vector<std::string>{"Jazz"});
    editor.tryHandleEvent(ftxui::Event::Return);
    CHECK(editor.takeRequest() == TrackEditorRequest::Apply);
    CHECK(editor.buildPatch().tagsToAdd == std::vector<std::string>{"new tag"});
  }

  TEST_CASE("TrackTagPopover - narrow results preserve readable names across intent changes",
            "[tui][regression][editor]")
  {
    for (std::int32_t const width : {24, 32})
    {
      auto editor = makeEditor(
        {{.title = "First", .album = "Album"}}, {}, {{"Progressive Rock", 1}}, {}, "en", TrackEditorMode::Tags);
      auto const initial = renderElement(editor.renderModal(width, 20), width, 20);
      CAPTURE(width, initial.text);
      CHECK(initial.text.contains("Progress"));
      CHECK(initial.text.contains("[x]"));
      editor.tryHandleEvent(ftxui::Event::Character(' '));
      auto const pending = renderElement(editor.renderModal(width, 20), width, 20);
      CHECK(pending.text.contains("Progress"));
      CHECK(pending.text.contains("[ ]"));
      CHECK(editor.buildPatch().tagsToRemove == std::vector<std::string>{"Progressive Rock"});
    }
  }
} // namespace ao::tui::test
