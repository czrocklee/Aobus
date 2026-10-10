// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/TrackEditController.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/runtime/AppRuntimeTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include "tui/TrackPropertiesEditor.h"
#include <ao/CoreIds.h>
#include <ao/library/Credits.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>

#include <array>
#include <bitset>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
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

    ftxui::Event applyEvent()
    {
      return ftxui::Event::CtrlS;
    }

    ftxui::Event reloadEvent()
    {
      return ftxui::Event::CtrlR;
    }

    struct EditFixture final
    {
      ao::test::TempDir tempDir{};
      rt::test::QueuedExecutor* executor = nullptr;
      std::unique_ptr<rt::AppRuntime> runtimePtr{rt::test::makeRuntime(tempDir, makeQueuedExecutor(executor))};
      std::size_t refreshCount = 0;
      std::size_t settledCount = 0;

      TrackId addTrack(library::test::TrackSpec const& spec) const
      {
        return rt::test::addRuntimeTrack(*runtimePtr, spec);
      }

      TrackEditController makeController()
      {
        return TrackEditController{runtimePtr->async(),
                                   runtimePtr->library(),
                                   runtimePtr->notifications(),
                                   ao::test::englishMessageCatalog(),
                                   TrackEditController::Outputs{
                                     .requestRefresh = [this] { ++refreshCount; },
                                     .notifySubmittedWriteSettled = [this] { ++settledCount; },
                                   },
                                   runtimePtr->completion(),
                                   runtimePtr->textOrderingPolicy()};
      }

      /// Commits an unrelated change, which invalidates every open binding.
      void commitUnrelatedChange() const
      {
        REQUIRE(rt::test::runRuntimeTask(
          *runtimePtr, runtimePtr->library().commands().createListAsync(rt::ListDraft{.name = "Other"})));
      }

      std::string lastMessage() const
      {
        auto const feed = runtimePtr->notifications().feed();
        REQUIRE_FALSE(feed.entries.empty());
        return std::get<std::string>(feed.entries.back().message);
      }

      bool hasNotifications() const { return !runtimePtr->notifications().feed().entries.empty(); }

      library::test::TrackSpec trackSpec(TrackId const trackId) const
      {
        return rt::test::runtimeTrackSpec(*runtimePtr, trackId);
      }
    };

    /**
     * @brief Moves row selection onto @p label's row.
     */
    void focusRowInput(TrackEditController& controller, std::string_view const label)
    {
      auto const spec = uimodel::buildTrackPropertiesFormSpec(ao::test::englishMessageCatalog());
      auto targetIndex = spec.metadataRows.size();

      for (std::size_t i = 0; i < spec.metadataRows.size(); ++i)
      {
        if (spec.metadataRows[i].label == label)
        {
          targetIndex = i;
          break;
        }
      }

      REQUIRE(targetIndex < spec.metadataRows.size());

      for (std::size_t step = 0; step < 50; ++step)
      {
        controller.tryHandleEvent(ftxui::Event::ArrowUp);
      }

      for (std::size_t step = 0; step < targetIndex; ++step)
      {
        controller.tryHandleEvent(ftxui::Event::ArrowDown);
      }
    }

    void typeText(TrackEditController& controller, std::string_view const text)
    {
      for (auto const character : text)
      {
        controller.tryHandleEvent(ftxui::Event::Character(character));
      }
    }

    /// Types @p value over @p label, which is what grants the row its Apply intent.
    void replaceField(TrackEditController& controller, std::string_view const label, std::string_view const value)
    {
      focusRowInput(controller, label);
      controller.tryHandleEvent(ftxui::Event::End);

      for (std::size_t step = 0; step < 64; ++step)
      {
        controller.tryHandleEvent(ftxui::Event::Backspace);
      }

      typeText(controller, value);
    }

    void clearField(TrackEditController& controller, std::string_view const label)
    {
      focusRowInput(controller, label);
      controller.tryHandleEvent(ftxui::Event::CtrlD);
    }
  } // namespace

  TEST_CASE("TrackEditController - opens over the whole captured selection", "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto const firstId = fixture.addTrack({.title = "First", .album = "Blue", .uri = "first.flac"});
    auto const secondId = fixture.addTrack({.title = "Second", .album = "Blue", .uri = "second.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpen({firstId, secondId}));
    REQUIRE(controller.isActive());

    auto const* const editor = controller.activeEditor();
    REQUIRE(editor != nullptr);
    REQUIRE(editor->targetCount() == 2);
    CHECK(editor->targets()[0].id == firstId);
    CHECK(editor->targets()[0].title == "First");
    CHECK_FALSE(editor->targets()[0].path.empty());
    CHECK(editor->targets()[1].id == secondId);
    CHECK(editor->targets()[1].title == "Second");
    CHECK(editor->status() == TrackEditorStatus::Ready);
    CHECK(fixture.refreshCount > 0);
  }

  TEST_CASE("TrackEditController - refuses to open without targets", "[tui][integration][editor]")
  {
    auto fixture = EditFixture{};
    auto controller = fixture.makeController();

    for (auto const mode : {TrackEditorMode::Properties, TrackEditorMode::Tags})
    {
      CHECK_FALSE(controller.tryOpen({}, mode));
      CHECK_FALSE(controller.isActive());
      CHECK(fixture.lastMessage() == "No track is selected");
    }
  }

  TEST_CASE("TrackEditController - refuses a selection it cannot open completely", "[tui][integration][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Present", .uri = "present.flac"});
    auto controller = fixture.makeController();

    CHECK_FALSE(controller.tryOpen({trackId, TrackId{9999}}));
    CHECK_FALSE(controller.isActive());
    CHECK(fixture.lastMessage().starts_with("The complete selection could not be opened"));
  }

  TEST_CASE("TrackEditController - a second open is refused while an editor is active", "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpen({trackId}));
    auto const* const editor = controller.activeEditor();

    CHECK_FALSE(controller.tryOpen({trackId}));
    CHECK(controller.activeEditor() == editor);
  }

  TEST_CASE("TrackEditController - open is refused after retirement", "[tui][integration][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .uri = "only.flac"});
    auto controller = fixture.makeController();

    controller.retire();

    CHECK_FALSE(controller.tryOpen({trackId}));
    CHECK_FALSE(controller.isActive());
    CHECK_FALSE(fixture.hasNotifications());
  }

  TEST_CASE("TrackEditController credits - invalid scopes do not install an editor or emit notifications",
            "[tui][unit][credits]")
  {
    using K = library::CreditKind;
    auto fixture = EditFixture{};
    auto const id = fixture.addTrack({.title = "Present", .uri = "present.flac"});
    auto controller = fixture.makeController();
    auto const refreshCount = fixture.refreshCount;
    auto const notificationCount = fixture.runtimePtr->notifications().feed().entries.size();

    for (auto const scope : {std::bitset<library::kCreditKindCount>{},
                             uimodel::trackCreditScope(K::Conductor) | uimodel::trackCreditScope(K::Soloist)})
    {
      CHECK_FALSE(controller.tryOpenCredits({id}, scope));
      CHECK(controller.activeEditor() == nullptr);
      CHECK_FALSE(controller.isActive());
      CHECK_FALSE(controller.hasPendingSubmission());
      CHECK(fixture.refreshCount == refreshCount);
      CHECK(fixture.runtimePtr->notifications().feed().entries.size() == notificationCount);
      CHECK(fixture.trackSpec(id).title == "Present");
    }
  }

  TEST_CASE("TrackEditController credits - no targets preserves the unopened state and existing warning",
            "[tui][integration][credits]")
  {
    auto fixture = EditFixture{};
    auto controller = fixture.makeController();
    CHECK_FALSE(controller.tryOpenCredits({}, uimodel::allTrackCreditKinds()));
    CHECK(controller.activeEditor() == nullptr);
    CHECK_FALSE(controller.isActive());
    CHECK_FALSE(controller.hasPendingSubmission());
    CHECK(fixture.refreshCount == 0);
    REQUIRE(fixture.runtimePtr->notifications().feed().entries.size() == 1);
    CHECK(fixture.lastMessage() == "No track is selected");
  }

  TEST_CASE("TrackEditController credits - missing target never installs a partial selection",
            "[tui][integration][credits]")
  {
    auto fixture = EditFixture{};
    auto const id = fixture.addTrack({.title = "Present", .uri = "present.flac"});
    auto controller = fixture.makeController();
    CHECK_FALSE(controller.tryOpenCredits({id, TrackId{9999}}, uimodel::allTrackCreditKinds()));
    CHECK(controller.activeEditor() == nullptr);
    CHECK_FALSE(controller.isActive());
    CHECK_FALSE(controller.hasPendingSubmission());
    CHECK(fixture.refreshCount == 0);
    REQUIRE(fixture.runtimePtr->notifications().feed().entries.size() == 1);
    CHECK(fixture.lastMessage().starts_with("The complete selection could not be opened"));
    CHECK(fixture.trackSpec(id).title == "Present");
  }

  TEST_CASE("TrackEditController credits - refused second open retains captured targets and dirty draft",
            "[tui][unit][credits]")
  {
    using K = library::CreditKind;
    auto fixture = EditFixture{};
    auto const first = fixture.addTrack({.title = "First", .album = "Old", .uri = "first.flac"});
    auto const second = fixture.addTrack({.title = "Second", .uri = "second.flac"});
    auto controller = fixture.makeController();
    REQUIRE(controller.tryOpen({first}));
    replaceField(controller, "Album", "Draft");
    auto const* const editor = controller.activeEditor();
    REQUIRE(editor != nullptr);
    REQUIRE(editor->isDirty());
    auto const refreshCount = fixture.refreshCount;
    auto const notificationCount = fixture.runtimePtr->notifications().feed().entries.size();

    for (auto const scope : {uimodel::allTrackCreditKinds(),
                             uimodel::trackCreditScope(K::Soloist),
                             std::bitset<library::kCreditKindCount>{}})
    {
      CHECK_FALSE(controller.tryOpenCredits({second}, scope));
      REQUIRE(controller.activeEditor() == editor);
      REQUIRE(editor->targetCount() == 1);
      CHECK(editor->targets()[0].id == first);
      CHECK(editor->status() == TrackEditorStatus::Ready);
      CHECK(editor->isDirty());
      CHECK_FALSE(editor->isEditingCredits());
      CHECK(editor->buildPatch().metadata.optAlbum == "Draft");
      CHECK_FALSE(controller.hasPendingSubmission());
      CHECK(fixture.refreshCount == refreshCount);
      CHECK(fixture.runtimePtr->notifications().feed().entries.size() == notificationCount);
    }

    CHECK(fixture.trackSpec(first).album == "Old");
    CHECK(fixture.trackSpec(second).title == "Second");
  }

  TEST_CASE("TrackEditController credits - refused second open retains the active child draft", "[tui][unit][credits]")
  {
    using K = library::CreditKind;
    auto fixture = EditFixture{};
    auto const first =
      fixture.addTrack({.title = "First", .credits = {{"A", K::Soloist, "Violin"}}, .uri = "first.flac"});
    auto const second = fixture.addTrack({.title = "Second", .uri = "second.flac"});
    auto controller = fixture.makeController();
    REQUIRE(controller.tryOpenCredits({first}, uimodel::trackCreditScope(K::Soloist)));
    controller.tryHandleEvent(ftxui::Event::End);
    typeText(controller, " draft");
    auto const* const editor = controller.activeEditor();
    auto const refreshCount = fixture.refreshCount;
    CHECK_FALSE(controller.tryOpenCredits({second}, uimodel::allTrackCreditKinds()));
    REQUIRE(controller.activeEditor() == editor);
    CHECK(editor->isEditingCredits());
    CHECK(editor->targets()[0].id == first);
    CHECK(editor->status() == TrackEditorStatus::Ready);
    CHECK_FALSE(editor->canApply());
    CHECK_FALSE(controller.hasPendingSubmission());
    CHECK(fixture.refreshCount == refreshCount);
    CHECK_FALSE(fixture.hasNotifications());
    controller.tryHandleEvent(ftxui::Event::CtrlS);
    REQUIRE_FALSE(editor->isEditingCredits());
    auto const patch = editor->buildPatch();
    REQUIRE(patch.metadata.optCredits);
    CHECK(patch.metadata.optCredits->kinds == uimodel::trackCreditScope(K::Soloist));
    CHECK(patch.metadata.optCredits->entries == std::vector<library::Credit>{{"A draft", K::Soloist, "Violin"}});
    CHECK(fixture.trackSpec(first).credits == std::vector<library::Credit>{{"A", K::Soloist, "Violin"}});
  }

  TEST_CASE("TrackEditController credits - retired controller rejects opening without presentation effects",
            "[tui][unit][credits]")
  {
    auto fixture = EditFixture{};
    auto const id = fixture.addTrack({.title = "Present", .uri = "present.flac"});
    auto controller = fixture.makeController();
    controller.retire();
    auto const refreshCount = fixture.refreshCount;
    CHECK_FALSE(controller.tryOpenCredits({id}, uimodel::allTrackCreditKinds()));
    CHECK(controller.activeEditor() == nullptr);
    CHECK_FALSE(controller.isActive());
    CHECK_FALSE(controller.hasPendingSubmission());
    CHECK(fixture.refreshCount == refreshCount);
    CHECK_FALSE(fixture.hasNotifications());
  }

  TEST_CASE("TrackEditController credits - full and single-kind scopes open over exactly the supplied targets",
            "[tui][unit][credits]")
  {
    using K = library::CreditKind;
    auto fixture = EditFixture{};
    auto const first = fixture.addTrack({.title = "First", .uri = "first.flac"});
    auto const second = fixture.addTrack({.title = "Second", .uri = "second.flac"});
    auto const scopes = std::array{uimodel::allTrackCreditKinds(),
                                   uimodel::trackCreditScope(K::Conductor),
                                   uimodel::trackCreditScope(K::Ensemble),
                                   uimodel::trackCreditScope(K::Soloist),
                                   uimodel::trackCreditScope(K::Performer)};

    for (auto const scope : scopes)
    {
      auto controller = fixture.makeController();
      REQUIRE(controller.tryOpenCredits({first, second}, scope));
      auto const* const editor = controller.activeEditor();
      REQUIRE(editor != nullptr);
      CHECK(editor->isEditingCredits());
      CHECK_FALSE(editor->canApply());
      CHECK_FALSE(editor->buildPatch().metadata.optCredits);
      REQUIRE(editor->targetCount() == 2);
      CHECK(editor->targets()[0].id == first);
      CHECK(editor->targets()[1].id == second);
      CHECK_FALSE(controller.hasPendingSubmission());
      CHECK_FALSE(fixture.hasNotifications());
      controller.tryHandleEvent(ftxui::Event::Tab);
      controller.tryHandleEvent(ftxui::Event::Tab);
      controller.tryHandleEvent(ftxui::Event::Tab);
      controller.tryHandleEvent(ftxui::Event::Return);
      typeText(controller, "New");
      controller.tryHandleEvent(ftxui::Event::CtrlS);
      REQUIRE_FALSE(editor->isEditingCredits());
      auto const patch = editor->buildPatch();
      REQUIRE(patch.metadata.optCredits);
      CHECK(patch.metadata.optCredits->kinds == scope);
      REQUIRE(patch.metadata.optCredits->entries.size() == 1);
      CHECK(patch.metadata.optCredits->entries[0].name == "New");
      CHECK(scope.test(static_cast<std::size_t>(patch.metadata.optCredits->entries[0].kind)));
      CHECK_FALSE(controller.hasPendingSubmission());
      CHECK(fixture.trackSpec(first).credits.empty());
      CHECK(fixture.trackSpec(second).credits.empty());
    }

    auto controller = fixture.makeController();
    REQUIRE(controller.tryOpenCredits({first}, uimodel::allTrackCreditKinds()));
    REQUIRE(controller.activeEditor()->targetCount() == 1);
    CHECK(controller.activeEditor()->targets()[0].id == first);
  }

  TEST_CASE("TrackEditController - Escape closes a clean editor", "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpen({trackId}));
    CHECK(controller.tryHandleEvent(ftxui::Event::Escape));

    CHECK_FALSE(controller.isActive());
    // The workspace is still hidden behind nothing, so a later key is not ours.
    CHECK_FALSE(controller.tryHandleEvent(ftxui::Event::Escape));
  }

  TEST_CASE("TrackEditController - Apply writes one patch to every captured target", "[tui][integration][editor]")
  {
    auto fixture = EditFixture{};
    auto const firstId = fixture.addTrack({.title = "First", .album = "Old", .uri = "first.flac"});
    auto const secondId = fixture.addTrack({.title = "Second", .album = "Older", .uri = "second.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpen({firstId, secondId}));
    replaceField(controller, "Album", "Blue");
    REQUIRE(controller.activeEditor()->canApply());

    CHECK(controller.tryHandleEvent(applyEvent()));
    CHECK(controller.activeEditor()->status() == TrackEditorStatus::Submitting);
    CHECK(controller.hasPendingSubmission());

    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));

    CHECK_FALSE(controller.isActive());
    CHECK(fixture.settledCount == 1);
    CHECK(fixture.lastMessage() == "Updated 2 tracks");
    CHECK(fixture.trackSpec(firstId).album == "Blue");
    CHECK(fixture.trackSpec(secondId).album == "Blue");
    // Only the included field is written; the rest keep each target's own value.
    CHECK(fixture.trackSpec(firstId).title == "First");
    CHECK(fixture.trackSpec(secondId).title == "Second");
  }

  TEST_CASE("TrackEditController - submission starts on the callback executor before writing",
            "[tui][integration][editor][concurrency]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .album = "Old", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpen({trackId}));
    replaceField(controller, "Album", "Blue");
    fixture.executor->drain();
    controller.tryHandleEvent(applyEvent());
    REQUIRE(fixture.executor->tryWaitUntilQueued());

    // The session must enter its owning executor before a worker can write.
    // Waiting until publication to hop back would already have changed storage.
    CHECK(fixture.trackSpec(trackId).album == "Old");
    CHECK(controller.hasPendingSubmission());
    CHECK(controller.activeEditor()->status() == TrackEditorStatus::Submitting);

    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));
    CHECK(fixture.trackSpec(trackId).album == "Blue");
    CHECK(fixture.settledCount == 1);
    CHECK_FALSE(controller.isActive());
  }

  TEST_CASE("TrackEditController - an included value every target already has reports no change",
            "[tui][integration][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .album = "", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpen({trackId}));
    clearField(controller, "Album");
    REQUIRE(controller.activeEditor()->canApply());
    controller.tryHandleEvent(applyEvent());
    REQUIRE(controller.hasPendingSubmission());
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));

    CHECK_FALSE(controller.isActive());
    CHECK(fixture.lastMessage() == "No changes were needed");
    CHECK(fixture.trackSpec(trackId).album.empty());
  }

  TEST_CASE("TrackEditController - an invalidated session goes stale and refuses to submit",
            "[tui][integration][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .album = "Old", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpen({trackId}));
    replaceField(controller, "Album", "Blue");
    fixture.commitUnrelatedChange();

    REQUIRE(controller.activeEditor() != nullptr);
    CHECK(controller.activeEditor()->status() == TrackEditorStatus::Stale);
    CHECK_FALSE(controller.activeEditor()->canApply());

    CHECK(controller.tryHandleEvent(applyEvent()));

    CHECK_FALSE(controller.hasPendingSubmission());
    CHECK(controller.activeEditor()->status() == TrackEditorStatus::Stale);
    CHECK(fixture.trackSpec(trackId).album == "Old");
  }

  TEST_CASE("TrackEditController - reload rebinds a stale editor and resets its draft", "[tui][integration][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .album = "Old", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpen({trackId}));
    replaceField(controller, "Album", "Blue");
    fixture.commitUnrelatedChange();
    REQUIRE(controller.activeEditor()->status() == TrackEditorStatus::Stale);

    // Reload is destructive while dirty, so it asks before discarding.
    controller.tryHandleEvent(reloadEvent());
    REQUIRE(controller.activeEditor()->isConfirmingReload());
    controller.tryHandleEvent(ftxui::Event::Return);

    REQUIRE(controller.activeEditor() != nullptr);
    CHECK(controller.activeEditor()->status() == TrackEditorStatus::Ready);
    CHECK_FALSE(controller.activeEditor()->isDirty());
    CHECK(controller.activeEditor()->targetCount() == 1);

    // The rebound session can write, which is the point of reloading.
    replaceField(controller, "Album", "Green");
    REQUIRE(controller.activeEditor()->canApply());
    controller.tryHandleEvent(applyEvent());
    REQUIRE(controller.hasPendingSubmission());
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));

    CHECK(fixture.trackSpec(trackId).album == "Green");
  }

  TEST_CASE("TrackEditController - a submitted write outlives the editor it came from",
            "[tui][integration][editor][concurrency]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .album = "Old", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpen({trackId}));
    replaceField(controller, "Album", "Blue");
    REQUIRE(controller.activeEditor()->canApply());
    controller.tryHandleEvent(applyEvent());
    REQUIRE(controller.hasPendingSubmission());

    // Graceful exit takes the editor away without waiting for the write.
    controller.retire();
    CHECK_FALSE(controller.isActive());
    CHECK(controller.hasPendingSubmission());

    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));

    CHECK(fixture.settledCount == 1);
    CHECK(fixture.trackSpec(trackId).album == "Blue");
    // Retirement suppresses late presentation, exactly as it does for scans.
    CHECK_FALSE(fixture.hasNotifications());
  }

  TEST_CASE("TrackEditController - unified submission writes metadata and tags, deduplicating changed tracks",
            "[tui][integration][editor]")
  {
    auto fixture = EditFixture{};
    auto const firstId = fixture.addTrack({.title = "First", .album = "Old", .uri = "first.flac", .tags = {"rock"}});
    auto const secondId = fixture.addTrack({.title = "Second", .album = "Older", .uri = "second.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpen({firstId, secondId}));

    // Edit metadata: replace Album with "Blue"
    replaceField(controller, "Album", "Blue");

    // Switch to Tags tab
    controller.tryHandleEvent(ftxui::Event::Tab);
    REQUIRE(controller.activeEditor()->tab() == TrackEditorTab::Tags);

    // The tag query is always live, so a new tag is named by typing it
    typeText(controller, "jazz");
    // Enter to add tag to all
    controller.tryHandleEvent(ftxui::Event::Return);

    REQUIRE(controller.activeEditor()->canApply());
    controller.tryHandleEvent(applyEvent());
    REQUIRE(controller.hasPendingSubmission());
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));

    CHECK_FALSE(controller.isActive());
    CHECK(fixture.settledCount == 1);
    CHECK(fixture.lastMessage() == "Updated 2 tracks");

    auto const firstSpec = fixture.trackSpec(firstId);
    CHECK(firstSpec.album == "Blue");
    CHECK(firstSpec.tags == std::vector<std::string>{"rock", "jazz"});

    auto const secondSpec = fixture.trackSpec(secondId);
    CHECK(secondSpec.album == "Blue");
    CHECK(secondSpec.tags == std::vector<std::string>{"jazz"});
  }

  TEST_CASE("TrackEditController - quick tags reload preserves mode and writes only captured targets",
            "[tui][integration][editor][concurrency]")
  {
    auto fixture = EditFixture{};
    auto const firstId = fixture.addTrack({.title = "First", .album = "Old", .uri = "first.flac", .tags = {"rock"}});
    auto const secondId = fixture.addTrack({.title = "Second", .album = "Other", .uri = "second.flac"});
    auto controller = fixture.makeController();
    REQUIRE(controller.tryOpen({firstId}, TrackEditorMode::Tags));
    typeText(controller, "discarded");
    fixture.commitUnrelatedChange();
    controller.tryHandleEvent(reloadEvent());
    REQUIRE(controller.isActive());
    CHECK(controller.activeEditor()->mode() == TrackEditorMode::Tags);
    CHECK_FALSE(controller.activeEditor()->isDirty());
    typeText(controller, "late night");
    controller.tryHandleEvent(ftxui::Event::Return);
    REQUIRE(controller.hasPendingSubmission());
    controller.tryHandleEvent(ftxui::Event::Escape);
    CHECK(controller.isActive());
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));
    CHECK_FALSE(controller.isActive());
    CHECK(fixture.trackSpec(firstId).tags == std::vector<std::string>{"rock", "late night"});
    CHECK(fixture.trackSpec(firstId).album == "Old");
    CHECK(fixture.trackSpec(secondId).tags.empty());
    CHECK(fixture.trackSpec(secondId).album == "Other");
  }

  TEST_CASE("TrackEditController credits - category preview stages one scope alongside tags and metadata",
            "[tui][integration][credits]")
  {
    using library::CreditKind;
    auto fixture = EditFixture{};
    auto const firstId =
      fixture.addTrack({.title = "First",
                        .album = "Old",
                        .credits = {{"A", CreditKind::Conductor, "guest"}, {"X", CreditKind::Performer, "piano"}},
                        .uri = "first.flac"});
    auto const secondId =
      fixture.addTrack({.title = "Second",
                        .album = "Old",
                        .credits = {{"A", CreditKind::Conductor, "guest"}, {"Y", CreditKind::Performer, "voice"}},
                        .uri = "second.flac"});
    auto controller = fixture.makeController();
    REQUIRE(controller.tryOpen({firstId, secondId}));
    replaceField(controller, "Album", "New");
    controller.tryHandleEvent(ftxui::Event::Tab);
    typeText(controller, "tag");
    controller.tryHandleEvent(ftxui::Event::Return);
    controller.tryHandleEvent(ftxui::Event::TabReverse);
    focusRowInput(controller, "Conductor");
    controller.tryHandleEvent(ftxui::Event::Return);
    REQUIRE(controller.activeEditor()->isEditingCredits());
    CHECK_FALSE(controller.activeEditor()->canApply());
    CHECK(controller.activeEditor()->buildPatch().tagsToAdd.empty());
    controller.tryHandleEvent(ftxui::Event::End);
    typeText(controller, "B");
    controller.tryHandleEvent(ftxui::Event::CtrlS);
    CHECK_FALSE(controller.hasPendingSubmission());
    REQUIRE_FALSE(controller.activeEditor()->isEditingCredits());
    REQUIRE(controller.activeEditor()->canApply());
    auto const patch = controller.activeEditor()->buildPatch();
    REQUIRE(patch.metadata.optCredits);
    CHECK(patch.metadata.optCredits->kinds == uimodel::trackCreditScope(CreditKind::Conductor));
    CHECK(patch.metadata.optCredits->entries == std::vector<library::Credit>{{"AB", CreditKind::Conductor, "guest"}});
    controller.tryHandleEvent(ftxui::Event::CtrlS);
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));
    CHECK(fixture.trackSpec(firstId).credits ==
          std::vector<library::Credit>{{"AB", CreditKind::Conductor, "guest"}, {"X", CreditKind::Performer, "piano"}});
    CHECK(fixture.trackSpec(secondId).credits ==
          std::vector<library::Credit>{{"AB", CreditKind::Conductor, "guest"}, {"Y", CreditKind::Performer, "voice"}});
    CHECK(fixture.trackSpec(firstId).album == "New");
    CHECK(fixture.trackSpec(secondId).tags == std::vector<std::string>{"tag"});
  }

  TEST_CASE("TrackEditController credits - focused entry retains stale child until confirmed reload",
            "[tui][integration][credits]")
  {
    using library::CreditKind;
    auto fixture = EditFixture{};
    auto const id =
      fixture.addTrack({.title = "Track", .credits = {{"A", CreditKind::Soloist, "violin"}}, .uri = "track.flac"});
    auto controller = fixture.makeController();
    REQUIRE(controller.tryOpenCredits({id}, uimodel::allTrackCreditKinds()));
    controller.tryHandleEvent(ftxui::Event::End);
    typeText(controller, " draft");
    fixture.commitUnrelatedChange();
    REQUIRE(controller.activeEditor()->status() == TrackEditorStatus::Stale);
    CHECK(controller.activeEditor()->isEditingCredits());
    controller.tryHandleEvent(ftxui::Event::CtrlR);
    REQUIRE(controller.activeEditor()->isConfirmingReload());
    controller.tryHandleEvent(ftxui::Event::Escape);
    CHECK(controller.activeEditor()->isEditingCredits());
    controller.tryHandleEvent(ftxui::Event::CtrlS);
    REQUIRE_FALSE(controller.activeEditor()->isEditingCredits());
    CHECK_FALSE(controller.activeEditor()->canApply());
    CHECK(controller.activeEditor()->buildPatch().metadata.optCredits->entries[0].name == "A draft");
    controller.tryHandleEvent(ftxui::Event::CtrlR);
    REQUIRE(controller.activeEditor()->isConfirmingReload());
    controller.tryHandleEvent(ftxui::Event::Return);
    REQUIRE(controller.activeEditor()->status() == TrackEditorStatus::Ready);
    CHECK_FALSE(controller.activeEditor()->isDirty());
    CHECK(fixture.trackSpec(id).credits == std::vector<library::Credit>{{"A", CreditKind::Soloist, "violin"}});
  }
} // namespace ao::tui::test
