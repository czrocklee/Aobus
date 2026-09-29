// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/SmartListEditor.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/library/list/SmartListEditing.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    /// A completion that always offers two tag insertions replacing the whole text.
    SmartListEditor::CompletionProvider tagCompletionProvider()
    {
      return [](std::string_view const text, std::size_t const) -> std::optional<rt::CompletionResult>
      {
        return rt::CompletionResult{
          .replaceBegin = 0,
          .replaceEnd = text.size(),
          .items =
            {
              rt::CompletionItem{.displayText = "#rock", .insertText = "#rock"},
              rt::CompletionItem{.displayText = "#jazz", .insertText = "#jazz"},
            },
        };
      };
    }

    SmartListEditor makeEditor(ListEditorMode const mode = ListEditorMode::New,
                               rt::ListDraft baseline = rt::ListDraft{},
                               std::string parentExpression = {},
                               SmartListEditor::CompletionProvider completionProvider = tagCompletionProvider())
    {
      return SmartListEditor{ao::test::englishMessageCatalog(),
                             mode,
                             std::move(baseline),
                             std::move(parentExpression),
                             std::move(completionProvider)};
    }

    /// A preview state a valid draft would produce.
    uimodel::SmartListEditorViewState validPreviewState()
    {
      auto state = uimodel::SmartListEditorViewState{};
      state.previewStatusText = "Showing all matches: 2";
      state.membershipEditingText = "Computed membership — edit tags or the expression";
      state.expressionValid = true;
      state.canSubmit = true;
      return state;
    }

    void typeText(SmartListEditor& editor, std::string_view const text)
    {
      for (auto const character : text)
      {
        editor.tryHandleEvent(ftxui::Event::Character(character));
      }
    }

    /// Moves the field focus by @p steps from the first field.
    void focusField(SmartListEditor& editor, std::size_t const steps)
    {
      for (std::size_t step = 0; step < 50; ++step)
      {
        editor.tryHandleEvent(ftxui::Event::ArrowUp);
      }

      for (std::size_t step = 0; step < steps; ++step)
      {
        editor.tryHandleEvent(ftxui::Event::ArrowDown);
      }
    }
  } // namespace

  TEST_CASE("SmartListEditor - starts from its baseline without pending intent", "[tui][unit][editor]")
  {
    auto baseline = rt::ListDraft{
      .parentId = ListId{7},
      .listId = ListId{9},
      .name = "Roadsongs",
      .description = "Live only",
      .expression = "#live",
    };
    auto editor = makeEditor(ListEditorMode::Edit, baseline);

    CHECK(editor.mode() == ListEditorMode::Edit);
    CHECK(editor.parentListId() == ListId{7});
    CHECK(editor.editListId() == ListId{9});
    CHECK_FALSE(editor.isDirty());
    CHECK(editor.takeRequest() == ListEditorRequest::None);

    auto const draft = editor.draft();
    CHECK(draft.parentId == ListId{7});
    CHECK(draft.listId == ListId{9});
    CHECK(draft.name == "Roadsongs");
    CHECK(draft.description == "Live only");
    CHECK(draft.expression == "#live");
  }

  TEST_CASE("SmartListEditor - a clean editor closes while a dirty one asks first", "[tui][unit][editor]")
  {
    auto editor = makeEditor();
    editor.applyPreview(validPreviewState(), {});

    CHECK(editor.tryHandleEvent(ftxui::Event::Escape));
    CHECK(editor.takeRequest() == ListEditorRequest::Close);
    CHECK_FALSE(editor.isConfirmingDiscard());

    auto dirty = makeEditor();
    dirty.applyPreview(validPreviewState(), {});
    focusField(dirty, 0);
    typeText(dirty, "Roadsongs");

    CHECK(dirty.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(dirty.takeRequest() == ListEditorRequest::Close);
    REQUIRE(dirty.isConfirmingDiscard());

    // A confirmation owns the keyboard, so unrelated keys change nothing.
    CHECK(dirty.tryHandleEvent(ftxui::Event::Character("x")));
    CHECK(dirty.isConfirmingDiscard());
    CHECK(dirty.isDirty());

    CHECK(dirty.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(dirty.isConfirmingDiscard());
    CHECK(dirty.isDirty());

    CHECK(dirty.tryHandleEvent(ftxui::Event::Escape));
    REQUIRE(dirty.isConfirmingDiscard());
    CHECK(dirty.tryHandleEvent(ftxui::Event::Return));
    CHECK(dirty.takeRequest() == ListEditorRequest::Close);
  }

  TEST_CASE("SmartListEditor - submit requires a name and a valid expression", "[tui][unit][editor]")
  {
    auto editor = makeEditor();
    editor.applyPreview(validPreviewState(), {});
    focusField(editor, 0);
    typeText(editor, "Roadsongs");

    REQUIRE(editor.isDirty());
    CHECK(editor.canSubmit());

    CHECK(editor.tryHandleEvent(ftxui::Event::CtrlS));
    CHECK(editor.takeRequest() == ListEditorRequest::Submit);

    // A view state the source rejected withholds the submit request entirely.
    auto invalid = uimodel::SmartListEditorViewState{};
    invalid.expressionValid = false;
    invalid.errorVisible = true;
    invalid.errorText = "Invalid filter: unbalanced (";
    editor.applyPreview(std::move(invalid), {});
    CHECK_FALSE(editor.canSubmit());
    CHECK(editor.tryHandleEvent(ftxui::Event::CtrlS));
    CHECK(editor.takeRequest() == ListEditorRequest::None);
  }

  TEST_CASE("SmartListEditor - an empty name withholds submission even with a valid expression", "[tui][unit][editor]")
  {
    auto editor = makeEditor(ListEditorMode::Edit, rt::ListDraft{.listId = ListId{4}, .name = "Named"});
    editor.applyPreview(validPreviewState(), {});
    focusField(editor, 0);

    for (std::size_t step = 0; step < 32; ++step)
    {
      editor.tryHandleEvent(ftxui::Event::Backspace);
    }

    // Clearing the name is an edit like any other; its owner consumes the
    // request before the refused submission would have anything to consume.
    CHECK(editor.takeRequest() == ListEditorRequest::PreviewChanged);
    CHECK_FALSE(editor.canSubmit());
    CHECK(editor.tryHandleEvent(ftxui::Event::CtrlS));
    CHECK(editor.takeRequest() == ListEditorRequest::None);
  }

  TEST_CASE("SmartListEditor - edits report the preview change they caused", "[tui][unit][editor]")
  {
    auto editor = makeEditor();
    editor.applyPreview(validPreviewState(), {});

    focusField(editor, 0);
    typeText(editor, "N");
    CHECK(editor.takeRequest() == ListEditorRequest::PreviewChanged);

    focusField(editor, 1);
    typeText(editor, "D");
    CHECK(editor.takeRequest() == ListEditorRequest::PreviewChanged);

    // A refused key reports nothing: no intent, no preview to recompute.
    focusField(editor, 2);
    editor.tryHandleEvent(ftxui::Event::Backspace);
    CHECK(editor.takeRequest() == ListEditorRequest::None);

    focusField(editor, 2);
    typeText(editor, "#");
    CHECK(editor.takeRequest() == ListEditorRequest::PreviewChanged);
  }

  TEST_CASE("SmartListEditor - arrows move fields and Return walks the form", "[tui][unit][editor]")
  {
    auto editor = makeEditor();
    editor.applyPreview(validPreviewState(), {});

    // The expression field is third; typing after two ArrowDown events edits it.
    editor.tryHandleEvent(ftxui::Event::ArrowDown);
    editor.tryHandleEvent(ftxui::Event::ArrowDown);
    typeText(editor, "#live");
    auto const draft = editor.draft();
    CHECK(draft.expression == "#live");
    CHECK(draft.name.empty());

    // Typing opened completion, so the first Return would accept a candidate;
    // Escape dismisses it, and the next Return wraps to the first field.
    CHECK(editor.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(editor.hasCompletion());
    CHECK(editor.tryHandleEvent(ftxui::Event::Return));
    typeText(editor, "N");
    CHECK(editor.draft().name == "N");
  }

  TEST_CASE("SmartListEditor - expression completion accepts candidates only for the expression", "[tui][unit][editor]")
  {
    auto editor = makeEditor();
    editor.applyPreview(validPreviewState(), {});

    // The name field never completes: Ctrl-N leaves the editor without a popup.
    focusField(editor, 0);
    typeText(editor, "Road");
    CHECK(editor.tryHandleEvent(ftxui::Event::CtrlN));
    CHECK_FALSE(editor.hasCompletion());

    focusField(editor, 2);
    typeText(editor, "#");
    CHECK(editor.tryHandleEvent(ftxui::Event::CtrlN));
    REQUIRE(editor.hasCompletion());

    CHECK(editor.tryHandleEvent(ftxui::Event::ArrowDown));
    CHECK(editor.tryHandleEvent(ftxui::Event::Return));
    CHECK_FALSE(editor.hasCompletion());
    CHECK(editor.draft().expression == "#jazz");
    CHECK(editor.takeRequest() == ListEditorRequest::PreviewChanged);

    // Escape inside a popup dismisses only the candidates.
    typeText(editor, " more");
    CHECK(editor.tryHandleEvent(ftxui::Event::CtrlN));
    REQUIRE(editor.hasCompletion());
    CHECK(editor.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(editor.hasCompletion());
    CHECK(editor.isDirty());
  }

  TEST_CASE("SmartListEditor - a submitting editor consumes every event", "[tui][unit][editor]")
  {
    auto editor = makeEditor();
    editor.applyPreview(validPreviewState(), {});
    focusField(editor, 0);
    typeText(editor, "Roadsongs");
    editor.setStatus(ListEditorStatus::Submitting);

    CHECK(editor.status() == ListEditorStatus::Submitting);
    CHECK(editor.tryHandleEvent(ftxui::Event::Escape));
    CHECK(editor.tryHandleEvent(ftxui::Event::CtrlS));
    CHECK(editor.tryHandleEvent(ftxui::Event::Character("x")));
    CHECK(editor.status() == ListEditorStatus::Submitting);
    CHECK(editor.takeRequest() == ListEditorRequest::None);
    CHECK(editor.isDirty());

    // The terminal result restores editing with its own diagnostic.
    editor.setStatus(ListEditorStatus::Ready, "Library is busy");
    CHECK(editor.status() == ListEditorStatus::Ready);
    CHECK(editor.diagnostic() == "Library is busy");
    CHECK(editor.canSubmit());
  }

  TEST_CASE("SmartListEditor - renders its fields, preview, and membership rows", "[tui][unit][editor]")
  {
    auto editor = makeEditor(ListEditorMode::Edit, rt::ListDraft{.listId = ListId{4}, .name = "Roadsongs"});
    auto state = validPreviewState();
    state.membershipEditingText = "Direct membership editing via #live";
    editor.applyPreview(state, {"First - Artist (Album)", "Second - Other"});

    auto const rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    CHECK(findTextCells(rendered.screen, "Edit List"));
    CHECK(findTextCells(rendered.screen, "Name"));
    CHECK(findTextCells(rendered.screen, "Description"));
    CHECK(findTextCells(rendered.screen, "Expression"));
    CHECK(findTextCells(rendered.screen, "Roadsongs"));
    CHECK(findTextCells(rendered.screen, "Showing all matches: 2"));
    CHECK(findTextCells(rendered.screen, "First - Artist (Album)"));
    CHECK(findTextCells(rendered.screen, "Second - Other"));
    CHECK(findTextCells(rendered.screen, "Direct membership editing via #live"));
    CHECK(findTextCells(rendered.screen, "Save"));
    // A library-root parent inherits nothing, so neither row appears.
    CHECK_FALSE(findTextCells(rendered.screen, "Inherited"));
    CHECK_FALSE(findTextCells(rendered.screen, "Effective"));
  }

  TEST_CASE("SmartListEditor - a filtered parent shows inherited and effective expressions", "[tui][unit][editor]")
  {
    auto baseline = rt::ListDraft{.listId = ListId{4}, .name = "Roadsongs", .expression = "#live"};
    auto editor = makeEditor(ListEditorMode::Edit, std::move(baseline), "#tour");
    editor.applyPreview(validPreviewState(), {});

    auto const rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    CHECK(findTextCells(rendered.screen, "Inherited"));
    CHECK(findTextCells(rendered.screen, "#tour"));
    CHECK(findTextCells(rendered.screen, "Effective"));
    CHECK(findTextCells(rendered.screen, "(#tour) and (#live)"));

    // The effective row follows the local expression as it is edited.
    focusField(editor, 2);
    typeText(editor, " and #bootleg");
    auto const edited = renderElement(editor.renderModal(80, 24), 80, 24);
    CHECK(findTextCells(edited.screen, "(#tour) and (#live and #bootleg)"));
  }

  TEST_CASE("SmartListEditor - renders the bounded preview its view state describes", "[tui][unit][editor]")
  {
    auto editor = makeEditor();
    auto state = uimodel::SmartListEditorViewState{};
    state.previewStatusText = "Showing 10 of 12 matches";
    state.expressionValid = true;
    state.canSubmit = true;
    editor.applyPreview(state, {"Only", "Two", "Three"});

    auto const rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    CHECK(findTextCells(rendered.screen, "Showing 10 of 12 matches"));
    CHECK(findTextCells(rendered.screen, "Only"));

    // An error the source reported replaces the preview with its diagnostic.
    auto invalid = uimodel::SmartListEditorViewState{};
    invalid.expressionValid = false;
    invalid.errorVisible = true;
    invalid.errorText = "Invalid filter";
    editor.applyPreview(std::move(invalid), {});
    auto const errorRendered = renderElement(editor.renderModal(80, 24), 80, 24);
    CHECK(findTextCells(errorRendered.screen, "Invalid filter"));
    CHECK_FALSE(findTextCells(errorRendered.screen, "Only"));
  }

  TEST_CASE("SmartListEditor - mouse input focuses fields and closes through the header control",
            "[tui][unit][mouse][editor]")
  {
    auto editor = makeEditor();
    editor.applyPreview(validPreviewState(), {});

    auto const rendered = renderElement(editor.renderModal(80, 24), 80, 24);
    auto const optExpressionBox = findTextCells(rendered.screen, "Expression");
    REQUIRE(optExpressionBox);

    auto const click = ftxui::Event::Mouse("",
                                           ftxui::Mouse{.button = ftxui::Mouse::Left,
                                                        .motion = ftxui::Mouse::Pressed,
                                                        .x = optExpressionBox->x_min,
                                                        .y = optExpressionBox->y_min});

    // A press no render has armed is consumed without acting on stale geometry.
    editor.tryHandleEvent(ftxui::Event::ArrowDown);
    REQUIRE(editor.tryHandleEvent(click));
    CHECK(editor.draft().expression.empty());

    [[maybe_unused]] auto const armed = renderElement(editor.renderModal(80, 24), 80, 24);
    REQUIRE(editor.tryHandleEvent(click));

    typeText(editor, "#live");
    CHECK(editor.draft().expression == "#live");
    CHECK(editor.draft().name.empty());

    // The header control follows the Escape protocol: it dismisses the active
    // completion first and only then asks a dirty editor to discard.
    CHECK(editor.hasCompletion());
    auto const dirtyRendered = renderElement(editor.renderModal(80, 24), 80, 24);
    auto const optCloseBox = findTextCells(dirtyRendered.screen, "×");
    REQUIRE(optCloseBox);
    auto const closeClick = ftxui::Event::Mouse("",
                                                ftxui::Mouse{.button = ftxui::Mouse::Left,
                                                             .motion = ftxui::Mouse::Pressed,
                                                             .x = optCloseBox->x_min,
                                                             .y = optCloseBox->y_min});
    REQUIRE(editor.tryHandleEvent(closeClick));
    CHECK_FALSE(editor.hasCompletion());
    CHECK_FALSE(editor.isConfirmingDiscard());

    [[maybe_unused]] auto const confirming = renderElement(editor.renderModal(80, 24), 80, 24);
    REQUIRE(editor.tryHandleEvent(closeClick));
    CHECK(editor.isConfirmingDiscard());
  }
} // namespace ao::tui::test
