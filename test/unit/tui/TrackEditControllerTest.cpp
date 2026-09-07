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
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>

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

    /// FTXUI delivers Ctrl-chords as their C0 control characters.
    ftxui::Event applyEvent()
    {
      return ftxui::Event::Character(static_cast<char>(0x13));
    }

    ftxui::Event reloadEvent()
    {
      return ftxui::Event::Character(static_cast<char>(0x12));
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
          *runtimePtr, runtimePtr->library().commands().createList(rt::ListDraft{.name = "Other"})));
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
      std::size_t targetIndex = 0;

      for (std::size_t i = 0; i < spec.metadataRows.size(); ++i)
      {
        if (spec.metadataRows[i].label == label)
        {
          targetIndex = i;
          break;
        }
      }

      for (std::size_t step = 0; step < 50; ++step)
      {
        controller.handleEvent(ftxui::Event::ArrowUp);
      }

      for (std::size_t step = 0; step < targetIndex; ++step)
      {
        controller.handleEvent(ftxui::Event::ArrowDown);
      }
    }

    void typeText(TrackEditController& controller, std::string_view const text)
    {
      for (auto const character : text)
      {
        controller.handleEvent(ftxui::Event::Character(character));
      }
    }

    /// Types @p value over @p label, which is what grants the row its Apply intent.
    void replaceField(TrackEditController& controller, std::string_view const label, std::string_view const value)
    {
      focusRowInput(controller, label);
      controller.handleEvent(ftxui::Event::End);

      for (std::size_t step = 0; step < 64; ++step)
      {
        controller.handleEvent(ftxui::Event::Backspace);
      }

      typeText(controller, value);
    }

    void clearField(TrackEditController& controller, std::string_view const label)
    {
      focusRowInput(controller, label);
      controller.handleEvent(ftxui::Event::Character(static_cast<char>(0x15)));
    }
  } // namespace

  TEST_CASE("TrackEditController - opens over the whole captured selection", "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto const firstId = fixture.addTrack({.title = "First", .album = "Blue", .uri = "first.flac"});
    auto const secondId = fixture.addTrack({.title = "Second", .album = "Blue", .uri = "second.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.open({firstId, secondId}));
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

  TEST_CASE("TrackEditController - refuses to open without targets", "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto controller = fixture.makeController();

    CHECK_FALSE(controller.open({}));
    CHECK_FALSE(controller.isActive());
    CHECK(fixture.lastMessage() == "No track is selected");
  }

  TEST_CASE("TrackEditController - refuses a selection it cannot open completely", "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Present", .uri = "present.flac"});
    auto controller = fixture.makeController();

    CHECK_FALSE(controller.open({trackId, TrackId{9999}}));
    CHECK_FALSE(controller.isActive());
    CHECK(fixture.lastMessage().starts_with("The complete selection could not be opened"));
  }

  TEST_CASE("TrackEditController - a second open is refused while an editor is active", "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.open({trackId}));
    auto const* const editor = controller.activeEditor();

    CHECK_FALSE(controller.open({trackId}));
    CHECK(controller.activeEditor() == editor);
  }

  TEST_CASE("TrackEditController - open is refused after retirement", "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .uri = "only.flac"});
    auto controller = fixture.makeController();

    controller.retire();

    CHECK_FALSE(controller.open({trackId}));
    CHECK_FALSE(controller.isActive());
    CHECK_FALSE(fixture.hasNotifications());
  }

  TEST_CASE("TrackEditController - Escape closes a clean editor", "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.open({trackId}));
    CHECK(controller.handleEvent(ftxui::Event::Escape));

    CHECK_FALSE(controller.isActive());
    // The workspace is still hidden behind nothing, so a later key is not ours.
    CHECK_FALSE(controller.handleEvent(ftxui::Event::Escape));
  }

  TEST_CASE("TrackEditController - Apply writes one patch to every captured target", "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto const firstId = fixture.addTrack({.title = "First", .album = "Old", .uri = "first.flac"});
    auto const secondId = fixture.addTrack({.title = "Second", .album = "Older", .uri = "second.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.open({firstId, secondId}));
    replaceField(controller, "Album", "Blue");
    REQUIRE(controller.activeEditor()->canApply());

    CHECK(controller.handleEvent(applyEvent()));
    CHECK(controller.activeEditor()->status() == TrackEditorStatus::Submitting);
    CHECK(controller.hasPendingSubmission());

    REQUIRE(fixture.executor->drainUntil([&] { return !controller.hasPendingSubmission(); }));

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
            "[tui][regression][editor][concurrency]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .album = "Old", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.open({trackId}));
    replaceField(controller, "Album", "Blue");
    fixture.executor->drain();
    controller.handleEvent(applyEvent());
    REQUIRE(fixture.executor->waitUntilQueued());

    // The session must enter its owning executor before a worker can write.
    // Waiting until publication to hop back would already have changed storage.
    CHECK(fixture.trackSpec(trackId).album == "Old");
    CHECK(controller.hasPendingSubmission());
    CHECK(controller.activeEditor()->status() == TrackEditorStatus::Submitting);

    REQUIRE(fixture.executor->drainUntil([&] { return !controller.hasPendingSubmission(); }));
    CHECK(fixture.trackSpec(trackId).album == "Blue");
    CHECK(fixture.settledCount == 1);
    CHECK_FALSE(controller.isActive());
  }

  TEST_CASE("TrackEditController - an included value every target already has reports no change", "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .album = "", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.open({trackId}));
    clearField(controller, "Album");
    REQUIRE(controller.activeEditor()->canApply());
    controller.handleEvent(applyEvent());
    REQUIRE(controller.hasPendingSubmission());
    REQUIRE(fixture.executor->drainUntil([&] { return !controller.hasPendingSubmission(); }));

    CHECK_FALSE(controller.isActive());
    CHECK(fixture.lastMessage() == "No changes were needed");
    CHECK(fixture.trackSpec(trackId).album.empty());
  }

  TEST_CASE("TrackEditController - an invalidated session goes stale and refuses to submit", "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .album = "Old", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.open({trackId}));
    replaceField(controller, "Album", "Blue");
    fixture.commitUnrelatedChange();

    REQUIRE(controller.activeEditor() != nullptr);
    CHECK(controller.activeEditor()->status() == TrackEditorStatus::Stale);
    CHECK_FALSE(controller.activeEditor()->canApply());

    CHECK(controller.handleEvent(applyEvent()));

    CHECK_FALSE(controller.hasPendingSubmission());
    CHECK(controller.activeEditor()->status() == TrackEditorStatus::Stale);
    CHECK(fixture.trackSpec(trackId).album == "Old");
  }

  TEST_CASE("TrackEditController - reload rebinds a stale editor and resets its draft", "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .album = "Old", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.open({trackId}));
    replaceField(controller, "Album", "Blue");
    fixture.commitUnrelatedChange();
    REQUIRE(controller.activeEditor()->status() == TrackEditorStatus::Stale);

    // Reload is destructive while dirty, so it asks before discarding.
    controller.handleEvent(reloadEvent());
    REQUIRE(controller.activeEditor()->isConfirmingReload());
    controller.handleEvent(ftxui::Event::Return);

    REQUIRE(controller.activeEditor() != nullptr);
    CHECK(controller.activeEditor()->status() == TrackEditorStatus::Ready);
    CHECK_FALSE(controller.activeEditor()->isDirty());
    CHECK(controller.activeEditor()->targetCount() == 1);

    // The rebound session can write, which is the point of reloading.
    replaceField(controller, "Album", "Green");
    REQUIRE(controller.activeEditor()->canApply());
    controller.handleEvent(applyEvent());
    REQUIRE(controller.hasPendingSubmission());
    REQUIRE(fixture.executor->drainUntil([&] { return !controller.hasPendingSubmission(); }));

    CHECK(fixture.trackSpec(trackId).album == "Green");
  }

  TEST_CASE("TrackEditController - a submitted write outlives the editor it came from",
            "[tui][unit][editor][concurrency]")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .album = "Old", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.open({trackId}));
    replaceField(controller, "Album", "Blue");
    REQUIRE(controller.activeEditor()->canApply());
    controller.handleEvent(applyEvent());
    REQUIRE(controller.hasPendingSubmission());

    // Graceful exit takes the editor away without waiting for the write.
    controller.retire();
    CHECK_FALSE(controller.isActive());
    CHECK(controller.hasPendingSubmission());

    REQUIRE(fixture.executor->drainUntil([&] { return !controller.hasPendingSubmission(); }));

    CHECK(fixture.settledCount == 1);
    CHECK(fixture.trackSpec(trackId).album == "Blue");
    // Retirement suppresses late presentation, exactly as it does for scans.
    CHECK_FALSE(fixture.hasNotifications());
  }

  TEST_CASE("TrackEditController - unified submission writes metadata and tags, deduplicating changed tracks",
            "[tui][unit][editor]")
  {
    auto fixture = EditFixture{};
    auto const firstId = fixture.addTrack({.title = "First", .album = "Old", .uri = "first.flac", .tags = {"rock"}});
    auto const secondId = fixture.addTrack({.title = "Second", .album = "Older", .uri = "second.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.open({firstId, secondId}));

    // Edit metadata: replace Album with "Blue"
    replaceField(controller, "Album", "Blue");

    // Switch to Tags tab
    controller.handleEvent(ftxui::Event::Tab);
    REQUIRE(controller.activeEditor()->tab() == TrackEditorTab::Tags);

    // The tag query is always live, so a new tag is named by typing it
    typeText(controller, "jazz");
    // Enter to add tag to all
    controller.handleEvent(ftxui::Event::Return);

    REQUIRE(controller.activeEditor()->canApply());
    controller.handleEvent(applyEvent());
    REQUIRE(controller.hasPendingSubmission());
    REQUIRE(fixture.executor->drainUntil([&] { return !controller.hasPendingSubmission(); }));

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
} // namespace ao::tui::test
