// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/ListAuthoringController.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include "tui/SmartListEditor.h"
#include <ao/CoreIds.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/uimodel/library/list/SmartListEditing.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    std::unique_ptr<async::Executor> makeQueuedExecutor(rt::test::QueuedExecutor*& executor)
    {
      auto ownerPtr = std::make_unique<rt::test::QueuedExecutor>();
      executor = ownerPtr.get();
      return ownerPtr;
    }

    struct AuthoringFixture final
    {
      ao::test::TempDir tempDir{};
      std::unique_ptr<rt::test::ControlledSleeper> sleeperPtr{std::make_unique<rt::test::ControlledSleeper>()};
      rt::test::QueuedExecutor* executor = nullptr;
      std::unique_ptr<rt::AppRuntime> runtimePtr{
        rt::test::makeRuntime(tempDir, makeQueuedExecutor(executor), nullptr, sleeperPtr.get())};
      uimodel::TrackPresentationCatalog presentationCatalog{runtimePtr->workspace(), ao::test::englishMessageCatalog()};
      uimodel::ListPresentations listPresentations{presentationCatalog, runtimePtr->library().changes()};
      std::size_t refreshCount = 0;
      std::size_t settledCount = 0;
      std::vector<ListId> openedLists{};

      ListAuthoringController makeController()
      {
        return ListAuthoringController{
          runtimePtr->async(),
          runtimePtr->library(),
          runtimePtr->views(),
          runtimePtr->sources(),
          runtimePtr->workspace(),
          runtimePtr->completion(),
          runtimePtr->notifications(),
          listPresentations,
          ao::test::englishMessageCatalog(),
          ListAuthoringController::Outputs{
            .requestRefresh = [this] { ++refreshCount; },
            .notifySubmittedWriteSettled = [this] { ++settledCount; },
            .openCreatedList = [this](ListId const listId) { openedLists.push_back(listId); },
          }};
      }

      TrackId addTrack(library::test::TrackSpec const& spec) const
      {
        return rt::test::addRuntimeTrack(*runtimePtr, spec);
      }

      ListId addList(std::string name, std::string expression = {}) const
      {
        return ao::test::requireValue(
          rt::test::runRuntimeTask(*runtimePtr,
                                   runtimePtr->library().commands().createListAsync(
                                     rt::ListDraft{.name = std::move(name), .expression = std::move(expression)})));
      }

      std::optional<rt::ListNode> listNode(ListId const listId) const
      {
        return runtimePtr->library().snapshot().listNode(listId);
      }

      std::string lastMessage() const
      {
        auto const feed = runtimePtr->notifications().feed();
        REQUIRE_FALSE(feed.entries.empty());
        return std::get<std::string>(feed.entries.back().message);
      }

      bool hasNotifications() const { return !runtimePtr->notifications().feed().entries.empty(); }
    };

    void typeText(ListAuthoringController& controller, std::string_view const text)
    {
      for (auto const character : text)
      {
        controller.tryHandleEvent(ftxui::Event::Character(character));
      }
    }

    /// Focuses the editor's field at @p index, counting from the Name row.
    void focusField(ListAuthoringController& controller, std::size_t const index)
    {
      for (std::size_t step = 0; step < 50; ++step)
      {
        controller.tryHandleEvent(ftxui::Event::ArrowUp);
      }

      for (std::size_t step = 0; step < index; ++step)
      {
        controller.tryHandleEvent(ftxui::Event::ArrowDown);
      }
    }

    /// Types @p name over the Name field, which is what makes Submit admissible.
    void draftNamedList(ListAuthoringController& controller, std::string_view const name)
    {
      focusField(controller, 0);

      for (std::size_t step = 0; step < 64; ++step)
      {
        controller.tryHandleEvent(ftxui::Event::Backspace);
      }

      typeText(controller, name);
    }

    /// Lands the deletion preview and draws the filled question once, which
    /// is what arms its keys, exactly as the shell's next frame would.
    void showDeleteQuestion(AuthoringFixture& fixture, ListAuthoringController& controller)
    {
      REQUIRE(fixture.executor->tryDrainUntil(
        [&]
        {
          auto const* const confirmation = controller.activeDeleteConfirmation();
          return confirmation == nullptr || confirmation->previewReady;
        }));
      REQUIRE(controller.activeDeleteConfirmation() != nullptr);
      std::ignore = renderElement(controller.activeModal(80, 24), 80, 24);
    }
  } // namespace

  TEST_CASE("ListAuthoringController - opens a new editor parented from the target", "[tui][unit][editor]")
  {
    auto fixture = AuthoringFixture{};
    [[maybe_unused]] auto const trackId = fixture.addTrack({.title = "Roadsongs", .uri = "road.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenNew(rt::kAllTracksListId));
    REQUIRE(controller.activeEditor() != nullptr);

    auto const* const editor = controller.activeEditor();
    CHECK(editor->mode() == ListEditorMode::New);
    CHECK(editor->parentListId() == kInvalidListId);
    CHECK(editor->editListId() == kInvalidListId);
    CHECK_FALSE(editor->isDirty());
    CHECK(fixture.refreshCount > 0);

    // The root parent previews the whole library through the shared state.
    CHECK(editor->viewState().isAllTracks);
    CHECK(editor->viewState().matchCount == 1);
    CHECK(editor->viewState().previewStatusText == "Showing all tracks: 1");
    REQUIRE(editor->previewTracks().size() == 1);
    CHECK(editor->previewTracks().front() == "Roadsongs - Artist (Album)");
  }

  TEST_CASE("ListAuthoringController - a real target parents the new List and previews its source",
            "[tui][integration][editor][concurrency]")
  {
    auto fixture = AuthoringFixture{};
    fixture.addTrack({.title = "First", .uri = "first.flac", .tags = {"rock"}});
    fixture.addTrack({.title = "Second", .uri = "second.flac", .tags = {"jazz"}});
    auto const parentId = fixture.addList("Parent");
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenNew(parentId));
    auto const* const editor = controller.activeEditor();
    CHECK(editor->parentListId() == parentId);
    CHECK_FALSE(editor->viewState().isAllTracks);
    CHECK(editor->viewState().matchCount == 2);

    // An expression drafts computed membership and narrows the live preview.
    focusField(controller, 2);
    typeText(controller, "#rock");
    // Each keystroke reschedules the debounce, so exactly one delay stays pending.
    REQUIRE(fixture.sleeperPtr->tryWaitForPendingDelays({std::chrono::milliseconds{200}}));
    REQUIRE(fixture.sleeperPtr->tryFireNext());
    REQUIRE(fixture.executor->tryDrainUntil([&] { return editor->viewState().matchCount == 1; }));

    CHECK(editor->viewState().previewStatusText == "Showing all matches: 1");
    REQUIRE(editor->previewTracks().size() == 1);
    CHECK(editor->previewTracks().front() == "First - Artist (Album)");
  }

  TEST_CASE("ListAuthoringController - a superseded preview generation never lands", "[tui][unit][editor][concurrency]")
  {
    auto fixture = AuthoringFixture{};
    fixture.addTrack({.title = "First", .uri = "first.flac", .tags = {"rock"}});
    fixture.addTrack({.title = "Second", .uri = "second.flac", .tags = {"jazz"}});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenNew(rt::kAllTracksListId));
    auto const* const editor = controller.activeEditor();

    focusField(controller, 2);
    typeText(controller, "#rock");
    typeText(controller, " and #jazz");

    // Every edit replaced its predecessor, so exactly one debounce stays
    // pending and firing it can only ever apply the newest draft.
    REQUIRE(fixture.sleeperPtr->tryWaitForPendingDelays({std::chrono::milliseconds{200}}));
    REQUIRE(fixture.sleeperPtr->tryFireNext());
    REQUIRE(fixture.executor->tryDrainUntil([&] { return editor->viewState().matchCount == 0; }));
    CHECK(editor->viewState().previewStatusText == "No matches");
    CHECK(editor->draft().expression == "#rock and #jazz");
  }

  TEST_CASE("ListAuthoringController - edit opens the saved definition exactly once", "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const listId = fixture.addList("Roadsongs", "#live");
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenEdit(listId));
    auto const* const editor = controller.activeEditor();
    CHECK(editor->mode() == ListEditorMode::Edit);
    CHECK(editor->editListId() == listId);
    auto const draft = editor->draft();
    CHECK(draft.name == "Roadsongs");
    CHECK(draft.expression == "#live");

    CHECK_FALSE(controller.tryOpenEdit(listId));
    CHECK(controller.activeEditor() == editor);
  }

  TEST_CASE("ListAuthoringController - a nested edit shows the parent's inherited expression",
            "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const parentId = fixture.addList("Tours", "#tour");
    auto const childId = ao::test::requireValue(
      rt::test::runRuntimeTask(*fixture.runtimePtr,
                               fixture.runtimePtr->library().commands().createListAsync(
                                 rt::ListDraft{.parentId = parentId, .name = "Roadsongs", .expression = "#live"})));
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenEdit(childId));

    auto const rendered = renderElement(controller.activeModal(80, 24), 80, 24);
    CHECK(findTextCells(rendered.screen, "Inherited"));
    CHECK(findTextCells(rendered.screen, "#tour"));
    CHECK(findTextCells(rendered.screen, "Effective"));
    CHECK(findTextCells(rendered.screen, "(#tour) and (#live)"));
  }

  TEST_CASE("ListAuthoringController - virtual targets refuse edit and new keeps the root parent",
            "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto controller = fixture.makeController();

    CHECK_FALSE(controller.tryOpenEdit(rt::kAllTracksListId));
    CHECK_FALSE(controller.tryOpenEdit(kInvalidListId));
    CHECK_FALSE(controller.isActive());
    CHECK(fixture.lastMessage() == "All Tracks cannot be edited or deleted");

    // New is the one action a virtual target admits, by parenting at the root.
    REQUIRE(controller.tryOpenNew(kInvalidListId));
    CHECK(controller.activeEditor()->parentListId() == kInvalidListId);
  }

  TEST_CASE("ListAuthoringController - create commits the draft and records the Auto presentation",
            "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    fixture.addTrack({.title = "Roadsongs", .uri = "road.flac", .tags = {"live"}});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenNew(rt::kAllTracksListId));
    draftNamedList(controller, "Live set");
    focusField(controller, 2);
    typeText(controller, "#live");
    REQUIRE(controller.activeEditor()->canSubmit());

    CHECK(controller.tryHandleEvent(ftxui::Event::CtrlS));
    CHECK(controller.hasPendingSubmission());
    CHECK(controller.activeEditor()->status() == ListEditorStatus::Submitting);
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));

    CHECK_FALSE(controller.isActive());
    CHECK(fixture.settledCount == 1);

    auto const optNode = fixture.runtimePtr->library().snapshot().lists();
    REQUIRE_FALSE(optNode.empty());

    auto savedIt = std::ranges::find_if(optNode, [](rt::ListNode const& node) { return node.name == "Live set"; });
    REQUIRE(savedIt != optNode.end());
    CHECK(savedIt->expression == "#live");
    CHECK(savedIt->parentId == kInvalidListId);

    // The Auto resolution is what the saved List's presentation preference holds.
    auto const optPresentationId = fixture.listPresentations.presentationIdForList(savedIt->id);
    REQUIRE(optPresentationId);
    CHECK(*optPresentationId ==
          uimodel::resolveSmartListTrackPresentationId(uimodel::kSmartListAutoTrackPresentationIndex,
                                                       true,
                                                       "#live",
                                                       rt::builtinTrackPresentationPresets(),
                                                       fixture.runtimePtr->workspace().customPresets()));
  }

  TEST_CASE("ListAuthoringController - update rewrites the definition and keeps the List identity",
            "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const listId = fixture.addList("Roadsongs", "#live");
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenEdit(listId));
    draftNamedList(controller, "Roadsongs 2026");
    focusField(controller, 2);
    typeText(controller, " and #bootleg");

    CHECK(controller.tryHandleEvent(ftxui::Event::CtrlS));
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));
    CHECK_FALSE(controller.isActive());

    auto const optNode = fixture.listNode(listId);
    REQUIRE(optNode);
    CHECK(optNode->name == "Roadsongs 2026");
    CHECK(optNode->expression == "#live and #bootleg");

    // An edit without a stored choice resolves the Auto presentation.
    auto const optPresentationId = fixture.listPresentations.presentationIdForList(listId);
    REQUIRE(optPresentationId);
    CHECK(*optPresentationId ==
          uimodel::resolveSmartListTrackPresentationId(uimodel::kSmartListAutoTrackPresentationIndex,
                                                       true,
                                                       "#live and #bootleg",
                                                       rt::builtinTrackPresentationPresets(),
                                                       fixture.runtimePtr->workspace().customPresets()));

    // Editing rewrote exactly one List; no sibling appeared beside it.
    CHECK(fixture.runtimePtr->library().snapshot().lists().size() == 1);
  }

  TEST_CASE("ListAuthoringController - an edit keeps the stored presentation choice", "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const listId = fixture.addList("Roadsongs", "#live");
    fixture.listPresentations.setPresentationIdForList(listId, "albums");
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenEdit(listId));
    draftNamedList(controller, "Roadsongs 2026");

    CHECK(controller.tryHandleEvent(ftxui::Event::CtrlS));
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));
    CHECK_FALSE(controller.isActive());

    // The stored choice survives the save instead of the Auto recommendation.
    auto const optPresentationId = fixture.listPresentations.presentationIdForList(listId);
    REQUIRE(optPresentationId);
    CHECK(*optPresentationId == "albums");
  }

  TEST_CASE("ListAuthoringController - a save failure keeps the draft for retry", "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const listId = fixture.addList("Roadsongs");
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenEdit(listId));
    draftNamedList(controller, "Renamed");

    // The target disappears while its editor is open; the retry then fails.
    std::ignore = ao::test::requireValue(rt::test::runRuntimeTask(
      *fixture.runtimePtr, fixture.runtimePtr->library().commands().deleteListAsync(listId, rt::DeleteListOptions{})));

    CHECK(controller.tryHandleEvent(ftxui::Event::CtrlS));
    REQUIRE(controller.hasPendingSubmission());
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));

    REQUIRE(controller.activeEditor() != nullptr);
    CHECK(controller.activeEditor()->status() == ListEditorStatus::Ready);
    CHECK_FALSE(controller.activeEditor()->diagnostic().empty());
    CHECK(controller.activeEditor()->isDirty());
    CHECK(fixture.settledCount == 1);
  }

  TEST_CASE("ListAuthoringController - a fast save revalidates the draft the debounce has not seen",
            "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    fixture.addTrack({.title = "First", .uri = "first.flac", .tags = {"rock"}});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenNew(rt::kAllTracksListId));
    auto const* const editor = controller.activeEditor();

    // Land one valid preview, then break the expression and submit before the
    // 200 ms debounce can run.
    focusField(controller, 0);
    typeText(controller, "Named");
    focusField(controller, 2);
    typeText(controller, "#rock");
    REQUIRE(fixture.sleeperPtr->tryWaitForPendingDelays({std::chrono::milliseconds{200}}));
    REQUIRE(fixture.sleeperPtr->tryFireNext());
    REQUIRE(fixture.executor->tryDrainUntil([&] { return editor->viewState().expressionValid; }));

    typeText(controller, " (");
    CHECK(controller.tryHandleEvent(ftxui::Event::CtrlS));

    // The submit recomputed the preview from the broken draft: no save left,
    // and the editor shows the draft's own diagnostic instead of the stale
    // validity and preview of the earlier text.
    CHECK_FALSE(controller.hasPendingSubmission());
    REQUIRE(controller.activeEditor() != nullptr);
    CHECK_FALSE(controller.activeEditor()->viewState().expressionValid);
    CHECK(controller.activeEditor()->viewState().errorVisible);
    CHECK_FALSE(controller.activeEditor()->viewState().errorText.empty());
    CHECK(controller.activeEditor()->status() == ListEditorStatus::Ready);
  }

  TEST_CASE("ListAuthoringController - a submitted save outlives the editor it came from",
            "[tui][integration][editor][concurrency]")
  {
    auto fixture = AuthoringFixture{};
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenNew(rt::kAllTracksListId));
    draftNamedList(controller, "Outliving");
    CHECK(controller.tryHandleEvent(ftxui::Event::CtrlS));
    REQUIRE(controller.hasPendingSubmission());

    // Graceful exit takes the surface away without waiting for the write.
    controller.retire();
    CHECK_FALSE(controller.isActive());
    CHECK(controller.hasPendingSubmission());

    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));

    CHECK(fixture.settledCount == 1);
    // Retirement suppresses late presentation, exactly as it does for scans.
    CHECK_FALSE(fixture.hasNotifications());

    auto const lists = fixture.runtimePtr->library().snapshot().lists();
    REQUIRE(lists.size() == 1);
    CHECK(lists.front().name == "Outliving");
  }

  TEST_CASE("ListAuthoringController - open is refused after retirement", "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto controller = fixture.makeController();
    controller.retire();

    CHECK_FALSE(controller.tryOpenNew(rt::kAllTracksListId));
    CHECK_FALSE(controller.tryOpenEdit(fixture.addList("Any")));
    CHECK_FALSE(controller.isActive());
    CHECK_FALSE(fixture.hasNotifications());
  }

  TEST_CASE("ListAuthoringController - an open editor answers for every event", "[tui][unit][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenNew(rt::kAllTracksListId));
    CHECK(controller.tryHandleEvent(ftxui::Event::Character("x")));
    CHECK(controller.tryHandleEvent(ftxui::Event::Home));
    CHECK(controller.isActive());

    // Escape asks about the edited draft, Return confirms the discard, and the
    // next event belongs to the workspace again.
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    REQUIRE(controller.activeEditor()->isConfirmingDiscard());
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK_FALSE(controller.isActive());
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Escape));
  }

  TEST_CASE("ListAuthoringController - delete previews the target before asking once", "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const listId = fixture.addList("Roadsongs", "#live");
    auto controller = fixture.makeController();

    // The confirmation owns input from the command on, while the preview
    // runs, and one flow exists at a time.
    REQUIRE(controller.tryDelete(listId));
    CHECK(controller.isActive());
    REQUIRE(controller.activeDeleteConfirmation() != nullptr);
    CHECK_FALSE(controller.activeDeleteConfirmation()->previewReady);
    CHECK(controller.activeDeleteConfirmation()->title == "Delete List?");
    CHECK_FALSE(controller.tryDelete(listId));
    CHECK_FALSE(controller.tryOpenNew(rt::kAllTracksListId));

    showDeleteQuestion(fixture, controller);

    auto const* const confirmation = controller.activeDeleteConfirmation();
    CHECK(confirmation->listId == listId);
    CHECK_FALSE(confirmation->includeDescendants);
    CHECK(confirmation->question.contains("\"Roadsongs\""));

    // Escape cancels without deleting, and the flow is gone.
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(controller.isActive());
    CHECK(fixture.listNode(listId).has_value());
  }

  TEST_CASE("ListAuthoringController - delete commits after one confirmation", "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const listId = fixture.addList("Roadsongs");
    auto controller = fixture.makeController();

    REQUIRE(controller.tryDelete(listId));
    showDeleteQuestion(fixture, controller);

    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK(controller.hasPendingSubmission());
    CHECK(controller.activeDeleteConfirmation()->deleting);

    // A confirming surface consumes everything until its write settles.
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));

    CHECK_FALSE(controller.isActive());
    CHECK_FALSE(fixture.listNode(listId).has_value());
    CHECK(fixture.settledCount == 1);
  }

  TEST_CASE("ListAuthoringController - a subtree deletion previews every descendant it will remove",
            "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const parentId = fixture.addList("Tours");
    auto const childDraft = rt::ListDraft{.parentId = parentId, .name = "2026"};
    auto const childId = ao::test::requireValue(rt::test::runRuntimeTask(
      *fixture.runtimePtr, fixture.runtimePtr->library().commands().createListAsync(childDraft)));
    auto controller = fixture.makeController();

    REQUIRE(controller.tryDelete(parentId));
    showDeleteQuestion(fixture, controller);

    auto const* const confirmation = controller.activeDeleteConfirmation();
    REQUIRE(confirmation != nullptr);
    CHECK(confirmation->includeDescendants);
    CHECK(confirmation->title == "Delete List and Descendants?");
    REQUIRE(confirmation->deletedListNames.size() == 2);
    CHECK(confirmation->deletedListNames[0] == "Tours");
    CHECK(confirmation->deletedListNames[1] == "2026");

    // The subtree question names both removed Lists through the modal.
    auto const rendered = renderElement(controller.activeModal(80, 24), 80, 24);
    CHECK(findTextCells(rendered.screen, "Delete 2 Lists in this derived subtree?"));
    CHECK(findTextCells(rendered.screen, "Tours"));
    CHECK(findTextCells(rendered.screen, "2026"));

    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));

    CHECK_FALSE(fixture.listNode(parentId).has_value());
    CHECK_FALSE(fixture.listNode(childId).has_value());
  }

  TEST_CASE("ListAuthoringController - a writable membership tag offers track cleanup on delete",
            "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const trackId = fixture.addTrack({.title = "Roadsongs", .uri = "road.flac", .tags = {"live"}});
    auto const listId = fixture.addList("Roadsongs", "#live");
    auto controller = fixture.makeController();

    REQUIRE(controller.tryDelete(listId));
    showDeleteQuestion(fixture, controller);

    auto const* const confirmation = controller.activeDeleteConfirmation();
    REQUIRE(confirmation != nullptr);
    CHECK_FALSE(confirmation->tagImpactQuestion.empty());
    CHECK(confirmation->tagImpactQuestion.contains("#live"));
    CHECK_FALSE(confirmation->removeWritableTag);

    // Space toggles the cleanup offer, and the deletion applies the choice.
    CHECK(controller.tryHandleEvent(ftxui::Event::Character(" ")));
    REQUIRE(controller.activeDeleteConfirmation()->removeWritableTag);
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));

    CHECK_FALSE(fixture.listNode(listId).has_value());
    auto const spec = rt::test::runtimeTrackSpec(*fixture.runtimePtr, trackId);
    CHECK(spec.tags.empty());
  }

  TEST_CASE("ListAuthoringController - a widely referenced tag keeps its warning and footer on screen",
            "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    fixture.addTrack({.title = "Roadsongs", .uri = "road.flac", .tags = {"live"}});
    auto const targetId = fixture.addList("Target", "#live");
    fixture.addList("Alpha", "#live");
    fixture.addList("Beta", "#live");
    fixture.addList("Gamma", "#live");
    fixture.addList("Delta", "#live");
    auto controller = fixture.makeController();

    REQUIRE(controller.tryDelete(targetId));
    showDeleteQuestion(fixture, controller);

    auto const& confirmation = *controller.activeDeleteConfirmation();
    REQUIRE_FALSE(confirmation.tagReferencesWarning.empty());

    // The warning names a bounded window of references and counts the rest
    // instead of wrapping to an unbounded height.
    CHECK(confirmation.tagReferencesWarning.contains("Alpha"));
    CHECK(confirmation.tagReferencesWarning.contains("+2 more Lists"));
    CHECK_FALSE(confirmation.tagReferencesWarning.contains("Delta"));

    // At a constrained terminal the measured warning budget keeps the footer
    // chips on screen next to the bounded warning.
    auto const rendered = renderElement(controller.activeModal(80, 20), 80, 20);
    CHECK(findTextCells(rendered.screen, "+2 more Lists"));
    CHECK(findTextCells(rendered.screen, "Enter Delete"));
    CHECK(findTextCells(rendered.screen, "Esc cancel"));
  }

  TEST_CASE("ListAuthoringController - a large subtree confirmation keeps its footer on screen",
            "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const parentId = fixture.addList("Tours");

    for (std::size_t index = 0; index < 12; ++index)
    {
      auto const childDraft = rt::ListDraft{.parentId = parentId, .name = std::format("Stop {:02d}", index)};
      std::ignore = ao::test::requireValue(rt::test::runRuntimeTask(
        *fixture.runtimePtr, fixture.runtimePtr->library().commands().createListAsync(childDraft)));
    }

    auto controller = fixture.makeController();

    REQUIRE(controller.tryDelete(parentId));
    showDeleteQuestion(fixture, controller);
    REQUIRE(controller.activeDeleteConfirmation()->deletedListNames.size() == 13);

    // A normal terminal shows the bounded leading window plus the count line,
    // never one row per removed List.
    auto const normal = renderElement(controller.activeModal(80, 24), 80, 24);
    CHECK(findTextCells(normal.screen, "Tours"));
    CHECK(findTextCells(normal.screen, "Stop 00"));
    CHECK(findTextCells(normal.screen, "Stop 04"));
    CHECK_FALSE(findTextCells(normal.screen, "Stop 05"));
    CHECK(findTextCells(normal.screen, "+7 more Lists"));

    // A short terminal shrinks the window further, and the footer chips stay
    // on screen behind whatever rows remain.
    auto const small = renderElement(controller.activeModal(80, 16), 80, 16);
    CHECK(findTextCells(small.screen, "Tours"));
    CHECK(findTextCells(small.screen, "Stop 03"));
    CHECK_FALSE(findTextCells(small.screen, "Stop 04"));
    CHECK(findTextCells(small.screen, "+8 more Lists"));
    CHECK(findTextCells(small.screen, "Delete All"));
    CHECK(findTextCells(small.screen, "Esc cancel"));
  }

  TEST_CASE("ListAuthoringController - the confirmation footer chips accept mouse clicks", "[tui][unit][mouse][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const listId = fixture.addList("Roadsongs");
    auto controller = fixture.makeController();

    REQUIRE(controller.tryDelete(listId));
    showDeleteQuestion(fixture, controller);

    // The cancel chip follows the Escape protocol through the bound event.
    auto const cancelRendered = renderElement(controller.activeModal(80, 24), 80, 24);
    auto const optCloseChip = findTextCells(cancelRendered.screen, "cancel");
    REQUIRE(optCloseChip);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("",
                                                          ftxui::Mouse{.button = ftxui::Mouse::Left,
                                                                       .motion = ftxui::Mouse::Pressed,
                                                                       .x = optCloseChip->x_min,
                                                                       .y = optCloseChip->y_min})));
    CHECK_FALSE(controller.isActive());
    CHECK(fixture.listNode(listId).has_value());

    // The confirm chip admits the deletion through its bound event.
    REQUIRE(controller.tryDelete(listId));
    showDeleteQuestion(fixture, controller);
    auto const confirmRendered = renderElement(controller.activeModal(80, 24), 80, 24);
    auto const optDeleteChip = findTextCells(confirmRendered.screen, "Enter");
    REQUIRE(optDeleteChip);
    REQUIRE(controller.tryHandleEvent(ftxui::Event::Mouse("",
                                                          ftxui::Mouse{.button = ftxui::Mouse::Left,
                                                                       .motion = ftxui::Mouse::Pressed,
                                                                       .x = optDeleteChip->x_min,
                                                                       .y = optDeleteChip->y_min})));

    CHECK(controller.activeDeleteConfirmation()->deleting);
    CHECK(controller.hasPendingSubmission());
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));
    CHECK_FALSE(fixture.listNode(listId).has_value());
  }

  TEST_CASE("ListAuthoringController - a click drawn for an earlier question cannot answer a new one",
            "[tui][integration][mouse][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const vanishingId = fixture.addList("Vanishing");
    auto const keptId = fixture.addList("Kept");
    auto controller = fixture.makeController();
    std::ignore = ao::test::requireValue(rt::test::runRuntimeTask(
      *fixture.runtimePtr,
      fixture.runtimePtr->library().commands().deleteListAsync(vanishingId, rt::DeleteListOptions{})));

    // The waiting question is drawn with its cancel chip, then closes on its
    // own when the preview fails, without any event reaching it.
    REQUIRE(controller.tryDelete(vanishingId));
    auto const waiting = renderElement(controller.activeModal(80, 24), 80, 24);
    auto const optCancelChip = findTextCells(waiting.screen, "cancel");
    REQUIRE(optCancelChip);
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.isActive(); }));

    // The next question has not been drawn yet, so the old chip's cells are
    // not its answer.
    REQUIRE(controller.tryDelete(keptId));
    auto const click = ftxui::Event::Mouse("",
                                           ftxui::Mouse{.button = ftxui::Mouse::Left,
                                                        .motion = ftxui::Mouse::Pressed,
                                                        .x = optCancelChip->x_min,
                                                        .y = optCancelChip->y_min});
    CHECK(controller.tryHandleEvent(click));
    REQUIRE(controller.activeDeleteConfirmation() != nullptr);
    CHECK(controller.activeDeleteConfirmation()->listId == keptId);

    // Once drawn, the question answers its own chip.
    std::ignore = renderElement(controller.activeModal(80, 24), 80, 24);
    CHECK(controller.tryHandleEvent(click));
    CHECK_FALSE(controller.isActive());
    CHECK(fixture.listNode(keptId).has_value());
    fixture.executor->drain();
  }

  TEST_CASE("ListAuthoringController - a failed delete preview reports the runtime error", "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const listId = fixture.addList("Vanishing");
    auto controller = fixture.makeController();

    // The target disappears between the command and its preview.
    std::ignore = ao::test::requireValue(rt::test::runRuntimeTask(
      *fixture.runtimePtr, fixture.runtimePtr->library().commands().deleteListAsync(listId, rt::DeleteListOptions{})));

    REQUIRE(controller.tryDelete(listId));
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.isActive(); }));
    REQUIRE(fixture.hasNotifications());
    CHECK(fixture.lastMessage().starts_with("list not found: "));
  }

  TEST_CASE("ListAuthoringController - a key typed before the question was drawn cannot confirm it",
            "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const listId = fixture.addList("Roadsongs");
    auto controller = fixture.makeController();

    // A second Return typed right after the command reaches the waiting
    // confirmation, never the workspace, and cannot answer it.
    REQUIRE(controller.tryDelete(listId));
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK_FALSE(controller.hasPendingSubmission());

    // A preview that landed but was not drawn yet still cannot be confirmed.
    REQUIRE(fixture.executor->tryDrainUntil([&] { return controller.activeDeleteConfirmation()->previewReady; }));
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK_FALSE(controller.hasPendingSubmission());
    CHECK_FALSE(controller.activeDeleteConfirmation()->deleting);

    // Once the filled question has been drawn, Return answers it.
    std::ignore = renderElement(controller.activeModal(80, 24), 80, 24);
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    CHECK(controller.activeDeleteConfirmation()->deleting);
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));
    CHECK_FALSE(fixture.listNode(listId).has_value());
  }

  TEST_CASE("ListAuthoringController - a question cancelled while it waits ignores its late preview",
            "[tui][integration][editor][concurrency]")
  {
    auto fixture = AuthoringFixture{};
    auto const firstId = fixture.addList("First");
    auto const secondId = fixture.addList("Second");
    auto controller = fixture.makeController();

    // The waiting question shows only Escape, and Escape closes it at once.
    REQUIRE(controller.tryDelete(firstId));
    auto const waiting = renderElement(controller.activeModal(80, 24), 80, 24);
    CHECK(findTextCells(waiting.screen, "Esc cancel"));
    CHECK_FALSE(findTextCells(waiting.screen, "Enter"));
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(controller.isActive());

    // The orphaned preview still lands; it neither reopens the question nor
    // reports anything.
    std::ignore = fixture.executor->tryDrainUntil([] { return false; }, std::chrono::milliseconds{200});
    CHECK_FALSE(controller.isActive());
    CHECK_FALSE(fixture.hasNotifications());
    CHECK(fixture.listNode(firstId).has_value());

    // The next question is filled by its own preview only.
    REQUIRE(controller.tryDelete(secondId));
    showDeleteQuestion(fixture, controller);
    CHECK(controller.activeDeleteConfirmation()->listId == secondId);
    CHECK(controller.activeDeleteConfirmation()->question.contains("\"Second\""));
  }

  TEST_CASE("ListAuthoringController - an admitted deletion outlives retirement",
            "[tui][integration][editor][concurrency]")
  {
    auto fixture = AuthoringFixture{};
    auto const listId = fixture.addList("Outliving");
    auto controller = fixture.makeController();

    REQUIRE(controller.tryDelete(listId));
    showDeleteQuestion(fixture, controller);
    CHECK(controller.tryHandleEvent(ftxui::Event::Return));
    REQUIRE(controller.hasPendingSubmission());

    // Graceful exit takes the surface away and waits for the write it admitted.
    controller.retire();
    CHECK_FALSE(controller.isActive());
    CHECK(controller.hasPendingSubmission());

    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));
    CHECK(fixture.settledCount == 1);
    CHECK_FALSE(fixture.hasNotifications());
    CHECK_FALSE(fixture.listNode(listId).has_value());
  }

  TEST_CASE("ListAuthoringController - retirement while a preview runs leaves nothing behind",
            "[tui][integration][editor][concurrency]")
  {
    auto fixture = AuthoringFixture{};
    auto const listId = fixture.addList("Kept");
    auto controller = fixture.makeController();

    REQUIRE(controller.tryDelete(listId));
    controller.retire();
    CHECK_FALSE(controller.isActive());
    CHECK_FALSE(controller.hasPendingSubmission());

    // Give the orphaned preview every chance to land; nothing observable waits for it.
    std::ignore = fixture.executor->tryDrainUntil([] { return false; }, std::chrono::milliseconds{200});
    CHECK_FALSE(controller.isActive());
    CHECK_FALSE(fixture.hasNotifications());
    CHECK(fixture.listNode(listId).has_value());
  }

  TEST_CASE("ListAuthoringController - a fast save of a corrected draft is not held back by a stale rejection",
            "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenNew(rt::kAllTracksListId));
    auto const* const editor = controller.activeEditor();

    // Land a rejecting preview for a broken expression.
    draftNamedList(controller, "Corrected");
    focusField(controller, 2);
    typeText(controller, "#rock (");
    REQUIRE(fixture.sleeperPtr->tryWaitForPendingDelays({std::chrono::milliseconds{200}}));
    REQUIRE(fixture.sleeperPtr->tryFireNext());
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !editor->viewState().expressionValid; }));
    REQUIRE_FALSE(editor->canSubmit());

    // Repair it and save inside the debounce window: the submit judges the
    // repaired draft, not the rejection that is still on screen.
    controller.tryHandleEvent(ftxui::Event::Backspace);
    controller.tryHandleEvent(ftxui::Event::Backspace);
    CHECK(controller.tryHandleEvent(ftxui::Event::CtrlS));
    REQUIRE(controller.hasPendingSubmission());
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));

    auto const lists = fixture.runtimePtr->library().snapshot().lists();
    REQUIRE(lists.size() == 1);
    CHECK(lists.front().name == "Corrected");
    CHECK(lists.front().expression == "#rock");
  }

  TEST_CASE("ListAuthoringController - a created List is opened and an edited one is not", "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const existingId = fixture.addList("Existing");
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpenNew(rt::kAllTracksListId));
    draftNamedList(controller, "Fresh");
    CHECK(controller.tryHandleEvent(ftxui::Event::CtrlS));
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));

    auto const lists = fixture.runtimePtr->library().snapshot().lists();
    auto const freshIt = std::ranges::find_if(lists, [](rt::ListNode const& node) { return node.name == "Fresh"; });
    REQUIRE(freshIt != lists.end());
    CHECK(fixture.openedLists == std::vector<ListId>{freshIt->id});

    REQUIRE(controller.tryOpenEdit(existingId));
    draftNamedList(controller, "Existing, renamed");
    CHECK(controller.tryHandleEvent(ftxui::Event::CtrlS));
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));
    REQUIRE(fixture.listNode(existingId).has_value());
    CHECK(fixture.listNode(existingId)->name == "Existing, renamed");
    CHECK(fixture.openedLists.size() == 1);
  }
} // namespace ao::tui::test
