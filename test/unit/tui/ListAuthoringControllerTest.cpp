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

#include <algorithm>
#include <chrono>
#include <cstddef>
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
      std::size_t interactionsCancelledCount = 0;

      ListAuthoringController makeController()
      {
        return ListAuthoringController{runtimePtr->async(),
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
                                         .cancelTransientInteractions = [this] { ++interactionsCancelledCount; },
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
    auto const childId = ao::test::requireValue(rt::test::runRuntimeTask(
      *fixture.runtimePtr,
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

  TEST_CASE("ListAuthoringController - an edit keeps the stored presentation choice",
            "[tui][integration][editor]")
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

    // Nothing is on screen while the preview runs, and one flow exists at a time.
    REQUIRE(controller.tryDelete(listId));
    CHECK_FALSE(controller.isActive());
    CHECK(controller.isBusy());
    CHECK_FALSE(controller.tryDelete(listId));
    CHECK_FALSE(controller.tryOpenNew(rt::kAllTracksListId));

    REQUIRE(fixture.executor->tryDrainUntil([&] { return controller.isActive(); }));

    auto const* const confirmation = controller.activeDeleteConfirmation();
    REQUIRE(confirmation != nullptr);
    CHECK(confirmation->listId == listId);
    CHECK_FALSE(confirmation->includeDescendants);
    CHECK(confirmation->title == "Delete List?");
    CHECK(confirmation->question.contains("\"Roadsongs\""));
    CHECK(fixture.interactionsCancelledCount == 1);

    // Escape cancels without deleting, and the flow is gone.
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));
    CHECK_FALSE(controller.isActive());
    CHECK_FALSE(controller.isBusy());
    CHECK(fixture.listNode(listId).has_value());
  }

  TEST_CASE("ListAuthoringController - delete commits after one confirmation", "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const listId = fixture.addList("Roadsongs");
    auto controller = fixture.makeController();

    REQUIRE(controller.tryDelete(listId));
    REQUIRE(fixture.executor->tryDrainUntil([&] { return controller.isActive(); }));

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
    REQUIRE(fixture.executor->tryDrainUntil([&] { return controller.isActive(); }));

    auto const* const confirmation = controller.activeDeleteConfirmation();
    REQUIRE(confirmation != nullptr);
    CHECK(confirmation->includeDescendants);
    CHECK(confirmation->title == "Delete List and Descendants?");
    CHECK(confirmation->question.contains('2'));
    CHECK(confirmation->question.contains("Tours"));
    CHECK(confirmation->question.contains("2026"));

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
    REQUIRE(fixture.executor->tryDrainUntil([&] { return controller.isActive(); }));

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

  TEST_CASE("ListAuthoringController - a failed delete preview reports the runtime error", "[tui][integration][editor]")
  {
    auto fixture = AuthoringFixture{};
    auto const listId = fixture.addList("Vanishing");
    auto controller = fixture.makeController();

    // The target disappears between the command and its preview.
    std::ignore = ao::test::requireValue(rt::test::runRuntimeTask(
      *fixture.runtimePtr, fixture.runtimePtr->library().commands().deleteListAsync(listId, rt::DeleteListOptions{})));

    REQUIRE(controller.tryDelete(listId));
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.isBusy(); }));

    CHECK_FALSE(controller.isActive());
    REQUIRE(fixture.hasNotifications());
    CHECK_FALSE(fixture.lastMessage().empty());
  }
} // namespace ao::tui::test
