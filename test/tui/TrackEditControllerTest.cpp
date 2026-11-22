// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/TrackEditController.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/runtime/AppRuntimeTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include "test/tui/RenderTestSupport.h"
#include "tui/TrackPropertiesEditor.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/Credits.h>
#include <ao/library/RecordingDate.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/ordering/TextOrderingPolicy.h>
#include <ao/uimodel/library/detail/TrackCredits.h>
#include <ao/uimodel/library/property/TrackPropertiesFormSpec.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>

#include <array>
#include <bitset>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    class TagSuggestionOrderingPolicy final : public rt::TextOrderingPolicy
    {
    public:
      explicit TagSuggestionOrderingPolicy(bool const failUnkeyedTag = false)
        : _failUnkeyedTag{failUnkeyedTag}
      {
      }

      Result<> makeSortKeyInto(std::string& output, std::string_view const text) const override
      {
        if (_failUnkeyedTag && text == "Unkeyed")
        {
          output = "zzzz";
          return makeError(Error::Code::ResourceExhausted, "Could not derive key");
        }

        if (text == "ä")
        {
          output = "a";
        }
        else if (text == "ＡＢＣ")
        {
          output = "ABC";
        }
        else
        {
          output = text;
        }

        return {};
      }

    private:
      bool _failUnkeyedTag;
    };

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

  TEST_CASE("TrackEditController - opens over the whole captured selection")
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

  TEST_CASE("TrackEditController - opens tags with ranked suggestions outside the selection")
  {
    auto fixture = EditFixture{};
    auto const selectedId = fixture.addTrack({.tags = {"Chosen"}});
    fixture.addTrack({.tags = {"ABC", "ＡＢＣ", "Unkeyed", "ä", "z", "Most frequent"}});
    fixture.addTrack({.tags = {"Most frequent"}});
    auto const checkSuggestions =
      [&](rt::TextOrderingPolicy const* policy, std::initializer_list<std::string_view> const expectedOrder)
    {
      auto controller = fixture.makeController();
      controller.setTextOrderingPolicy(policy);
      REQUIRE(controller.tryOpen({selectedId}, TrackEditorMode::Tags));
      CHECK_FALSE(fixture.hasNotifications());

      auto const* const editor = controller.activeEditor();
      REQUIRE(editor != nullptr);
      auto const text = renderElement(editor->renderModal(80, 24), 80, 24).text;
      INFO(text);
      auto previousLine = lineIndexContaining(text, "[x] Chosen");
      REQUIRE(previousLine >= 0);
      CHECK_FALSE(text.contains("[ ] Chosen"));

      for (auto const tag : expectedOrder)
      {
        auto const line = lineIndexContaining(text, std::format("[ ] {}", tag));
        REQUIRE(line > previousLine);
        previousLine = line;
      }
    };

    SECTION("Locale keys break frequency ties, then raw spelling breaks key ties")
    {
      auto const policy = TagSuggestionOrderingPolicy{};
      checkSuggestions(&policy, {"Most frequent", "ABC", "ＡＢＣ", "Unkeyed", "ä", "z"});
    }

    SECTION("Without a policy the snapshot's raw spelling order breaks frequency ties")
    {
      checkSuggestions(nullptr, {"Most frequent", "ABC", "Unkeyed", "z", "ä", "ＡＢＣ"});
    }

    SECTION("A failed key cannot reorder a retained tag by its partial output")
    {
      auto const policy = TagSuggestionOrderingPolicy{true};
      checkSuggestions(&policy, {"Most frequent", "Unkeyed", "ABC", "ＡＢＣ", "ä", "z"});
    }
  }

  TEST_CASE("TrackEditController - refuses to open without targets")
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

  TEST_CASE("TrackEditController - refuses a selection it cannot open completely")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Present", .uri = "present.flac"});
    auto controller = fixture.makeController();

    CHECK_FALSE(controller.tryOpen({trackId, TrackId{9999}}));
    CHECK_FALSE(controller.isActive());
    CHECK(fixture.lastMessage().starts_with("The complete selection could not be opened"));
  }

  TEST_CASE("TrackEditController - a second open is refused while an editor is active")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .uri = "only.flac"});
    auto controller = fixture.makeController();

    REQUIRE(controller.tryOpen({trackId}));
    auto const* const editor = controller.activeEditor();

    CHECK_FALSE(controller.tryOpen({trackId}));
    CHECK(controller.activeEditor() == editor);
  }

  TEST_CASE("TrackEditController - open is refused after retirement")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.title = "Only", .uri = "only.flac"});
    auto controller = fixture.makeController();

    controller.retire();

    CHECK_FALSE(controller.tryOpen({trackId}));
    CHECK_FALSE(controller.isActive());
    CHECK_FALSE(fixture.hasNotifications());
  }

  TEST_CASE("TrackEditController credits - invalid scopes do not install an editor or emit notifications")
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

  TEST_CASE("TrackEditController credits - no targets preserves the unopened state and existing warning")
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

  TEST_CASE("TrackEditController credits - missing target never installs a partial selection")
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

  TEST_CASE("TrackEditController credits - refused second open retains captured targets and dirty draft")
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

  TEST_CASE("TrackEditController credits - refused second open retains the active child draft")
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

  TEST_CASE("TrackEditController credits - retired controller rejects opening without presentation effects")
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

  TEST_CASE("TrackEditController credits - full and single-kind scopes open over exactly the supplied targets")
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
      controller.tryHandleEvent(ftxui::Event::Return); // The empty scope starts on Add.
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

  TEST_CASE("TrackEditController - Escape closes a clean editor")
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

  TEST_CASE("TrackEditController - Apply writes one patch to every captured target")
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

  TEST_CASE("TrackEditController - batch scalar edits preserve originals unless explicitly replaced or cleared")
  {
    using library::RecordingDate;
    auto fixture = EditFixture{};
    auto const firstDate = RecordingDate{.year = 1981, .month = 5, .day = 12};
    auto const firstId =
      fixture.addTrack({.album = "Old", .recordingDate = firstDate, .uri = "first.flac", .year = 1959});
    auto const secondId = fixture.addTrack({.album = "Older", .uri = "second.flac", .year = 0});
    auto controller = fixture.makeController();
    REQUIRE(controller.tryOpen({firstId, secondId}));

    auto const replacements = std::array{std::pair{"Album", "Unified"},
                                         std::pair{"Artist", "New artist"},
                                         std::pair{"Year", "2025"},
                                         std::pair{"Recording Date", "2025-02"}};
    auto const checkOriginals = [&]
    {
      auto const first = fixture.trackSpec(firstId);
      auto const second = fixture.trackSpec(secondId);
      CHECK(first.album == "Old");
      CHECK(second.album == "Older");
      CHECK(first.artist == "Artist");
      CHECK(second.artist == "Artist");
      CHECK(first.year == 1959);
      CHECK(second.year == 0);
      CHECK(first.recordingDate == firstDate);
      CHECK_FALSE(second.recordingDate.isPresent());
    };
    auto const apply = [&]
    {
      REQUIRE(controller.activeEditor()->canApply());
      controller.tryHandleEvent(applyEvent());
      REQUIRE(controller.hasPendingSubmission());
      REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));
      CHECK_FALSE(controller.isActive());
      CHECK(fixture.settledCount == 1);
    };
    auto const applyGenre = [&]
    {
      replaceField(controller, "Genre", "Jazz");
      apply();
      CHECK(fixture.trackSpec(firstId).genre == "Jazz");
      CHECK(fixture.trackSpec(secondId).genre == "Jazz");
    };

    SECTION("Apply without an edit does not submit")
    {
      controller.tryHandleEvent(applyEvent());
      CHECK_FALSE(controller.hasPendingSubmission());
      checkOriginals();
    }

    SECTION("Editing a different field preserves the scalar baselines")
    {
      applyGenre();
      checkOriginals();
    }

    SECTION("Erasing temporary text withdraws replacement intent for common and mixed scalars")
    {
      for (auto const& [label, text] : replacements)
      {
        replaceField(controller, label, text);
        replaceField(controller, label, "");
      }

      CHECK_FALSE(controller.activeEditor()->isDirty());
      applyGenre();
      checkOriginals();
    }

    SECTION("Spaces alone are a literal text replacement, not a withdrawn edit")
    {
      replaceField(controller, "Album", "   ");
      apply();
      CHECK(fixture.trackSpec(firstId).album == "   ");
      CHECK(fixture.trackSpec(secondId).album == "   ");
    }

    SECTION("Typing the numeric and date absence sentinels does not clear a batch")
    {
      replaceField(controller, "Year", "0");
      replaceField(controller, "Recording Date", " ");

      CHECK_FALSE(controller.activeEditor()->canApply());
      applyGenre();
      checkOriginals();
    }

    SECTION("Restoring explicitly cleared fields withdraws clear intent")
    {
      for (auto const& [label, text] : replacements)
      {
        clearField(controller, label);
        controller.tryHandleEvent(ftxui::Event::CtrlG);
      }

      CHECK_FALSE(controller.activeEditor()->isDirty());
      applyGenre();
      checkOriginals();
    }

    SECTION("Discarding staged clears never writes them")
    {
      for (auto const& [label, text] : replacements)
      {
        clearField(controller, label);
      }

      checkOriginals();
      controller.tryHandleEvent(ftxui::Event::Escape);
      REQUIRE(controller.activeEditor()->isConfirmingDiscard());
      controller.tryHandleEvent(ftxui::Event::Return);
      CHECK_FALSE(controller.isActive());
      CHECK_FALSE(controller.hasPendingSubmission());
      checkOriginals();
    }

    SECTION("Applying staged clears writes typed absence to every target")
    {
      for (auto const& [label, text] : replacements)
      {
        clearField(controller, label);
      }

      checkOriginals();
      apply();

      for (auto const trackId : {firstId, secondId})
      {
        auto const track = fixture.trackSpec(trackId);
        CHECK(track.album.empty());
        CHECK(track.artist.empty());
        CHECK(track.year == 0);
        CHECK_FALSE(track.recordingDate.isPresent());
      }
    }
  }

  TEST_CASE("TrackEditController - common batch scalar no-ops do not count as pending edits")
  {
    auto fixture = EditFixture{};
    auto const firstId = fixture.addTrack({.title = "First", .album = "", .uri = "first.flac", .year = 2003});
    auto const secondId = fixture.addTrack({.title = "Second", .album = "", .uri = "second.flac", .year = 2003});
    auto controller = fixture.makeController();
    REQUIRE(controller.tryOpen({firstId, secondId}));
    auto const* const editor = controller.activeEditor();
    REQUIRE(editor != nullptr);

    auto const checkClean = [&]
    {
      CHECK_FALSE(editor->canApply());
      CHECK_FALSE(editor->isDirty());
      CHECK(editor->patchSummary() == TrackEditorPatchSummary{});
      CHECK_FALSE(editor->buildPatch().metadata.optAlbum);
      CHECK_FALSE(editor->buildPatch().metadata.optYear);

      controller.tryHandleEvent(ftxui::Event::Escape);
      CHECK_FALSE(controller.isActive());
      CHECK_FALSE(controller.hasPendingSubmission());
    };

    SECTION("Clearing an already empty album leaves no pending clear")
    {
      clearField(controller, "Album");
      CHECK_FALSE(renderElement(editor->render(), 80, 24).text.contains("Clear for all 2 tracks"));
      checkClean();
    }

    SECTION("An equivalent numeric spelling remains editable without a change to save")
    {
      replaceField(controller, "Year", "02003");
      CHECK(renderElement(editor->render(), 80, 24).text.contains("02003"));
      checkClean();
    }

    SECTION("Invalid input is still kept for correction")
    {
      replaceField(controller, "Year", "02003x");
      CHECK(editor->isDirty());
      CHECK_FALSE(editor->canApply());
      CHECK(renderElement(editor->render(), 80, 24).text.contains("02003x"));
      controller.tryHandleEvent(ftxui::Event::Escape);
      REQUIRE(editor->isConfirmingDiscard());
      controller.tryHandleEvent(ftxui::Event::Escape);
      controller.tryHandleEvent(ftxui::Event::Backspace);
      checkClean();
    }

    SECTION("No-ops do not withdraw a mixed-field replacement equal to the first track's original")
    {
      replaceField(controller, "Title", "First");
      clearField(controller, "Album");
      replaceField(controller, "Year", "02003");

      REQUIRE(editor->canApply());
      CHECK(editor->patchSummary() == TrackEditorPatchSummary{.fieldCount = 1});
      controller.tryHandleEvent(applyEvent());
      REQUIRE(controller.hasPendingSubmission());
      REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));
      CHECK_FALSE(controller.isActive());
      CHECK(fixture.settledCount == 1);
      CHECK(fixture.trackSpec(firstId).title == "First");
      CHECK(fixture.trackSpec(secondId).title == "First");
    }

    for (auto const trackId : {firstId, secondId})
    {
      auto const track = fixture.trackSpec(trackId);
      CHECK(track.album.empty());
      CHECK(track.year == 2003);
    }
  }

  TEST_CASE("TrackEditController - single-track scalars still clear when erased")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.album = "Old", .recordingDate = {2003, 5, 12}, .year = 2003});
    auto controller = fixture.makeController();
    REQUIRE(controller.tryOpen({trackId}));
    replaceField(controller, "Album", "");
    replaceField(controller, "Year", "");
    replaceField(controller, "Recording Date", "");
    REQUIRE(controller.activeEditor()->canApply());
    CHECK(controller.activeEditor()->patchSummary() == TrackEditorPatchSummary{.fieldCount = 3, .clearCount = 3});

    controller.tryHandleEvent(applyEvent());
    REQUIRE(controller.hasPendingSubmission());
    REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));
    CHECK_FALSE(controller.isActive());
    auto const track = fixture.trackSpec(trackId);
    CHECK(track.album.empty());
    CHECK(track.year == 0);
    CHECK_FALSE(track.recordingDate.isPresent());
  }

  TEST_CASE("TrackEditController - submission starts on the callback executor before writing", "[concurrency]")
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

  TEST_CASE("TrackEditController - equivalent single-track scalars do not count as pending edits")
  {
    auto fixture = EditFixture{};
    auto const originalDate = library::RecordingDate{.year = 2003, .month = 5, .day = 12};
    auto const trackId = fixture.addTrack({.recordingDate = originalDate, .year = 2003});
    auto controller = fixture.makeController();
    REQUIRE(controller.tryOpen({trackId}));
    auto const* const editor = controller.activeEditor();
    REQUIRE(editor != nullptr);

    auto const checkClean = [&]
    {
      CHECK_FALSE(editor->isDirty());
      CHECK_FALSE(editor->canApply());
      CHECK(editor->patchSummary() == TrackEditorPatchSummary{});
      CHECK_FALSE(editor->buildPatch().metadata.optYear);
      CHECK_FALSE(editor->buildPatch().metadata.optRecordingDate);
      controller.tryHandleEvent(applyEvent());
      CHECK_FALSE(controller.hasPendingSubmission());
    };

    SECTION("Equivalent numbers and dates do not require discard")
    {
      replaceField(controller, "Year", "02003");
      replaceField(controller, "Recording Date", " 2003-05-12 ");
      checkClean();
      controller.tryHandleEvent(ftxui::Event::Escape);
      CHECK_FALSE(controller.isActive());
      auto const track = fixture.trackSpec(trackId);
      CHECK(track.year == 2003);
      CHECK(track.recordingDate == originalDate);
    }

    SECTION("Correcting invalid input to an equivalent number leaves it editable for a real change")
    {
      replaceField(controller, "Recording Date", " 2003-05-12 ");
      replaceField(controller, "Year", "02003x");
      CHECK(editor->isDirty());
      CHECK_FALSE(editor->canApply());
      CHECK_FALSE(editor->buildPatch().metadata.optYear);
      controller.tryHandleEvent(ftxui::Event::Escape);
      REQUIRE(editor->isConfirmingDiscard());
      controller.tryHandleEvent(ftxui::Event::Escape);

      controller.tryHandleEvent(ftxui::Event::Backspace);
      checkClean();
      CHECK(renderElement(editor->render(), 80, 24).text.contains("02003"));
      controller.tryHandleEvent(ftxui::Event::Backspace);
      typeText(controller, "4");
      CHECK(renderElement(editor->render(), 80, 24).text.contains("02004"));
      REQUIRE(editor->canApply());
      CHECK(editor->patchSummary() == TrackEditorPatchSummary{.fieldCount = 1});
      CHECK(editor->buildPatch().metadata.optYear == 2004);
      CHECK_FALSE(editor->buildPatch().metadata.optRecordingDate);

      controller.tryHandleEvent(applyEvent());
      REQUIRE(controller.hasPendingSubmission());
      REQUIRE(fixture.executor->tryDrainUntil([&] { return !controller.hasPendingSubmission(); }));
      CHECK_FALSE(controller.isActive());
      auto const track = fixture.trackSpec(trackId);
      CHECK(track.year == 2004);
      CHECK(track.recordingDate == originalDate);
    }
  }

  TEST_CASE("TrackEditController - single-track input that leaves scalars absent does not count as an edit")
  {
    auto fixture = EditFixture{};
    auto const trackId = fixture.addTrack({.album = "", .year = 0});
    auto controller = fixture.makeController();
    REQUIRE(controller.tryOpen({trackId}));
    auto const* const editor = controller.activeEditor();
    REQUIRE(editor != nullptr);

    clearField(controller, "Album");
    replaceField(controller, "Year", "00");
    replaceField(controller, "Recording Date", " ");
    CHECK_FALSE(editor->isDirty());
    CHECK_FALSE(editor->canApply());
    CHECK(editor->patchSummary() == TrackEditorPatchSummary{});
    auto const patch = editor->buildPatch().metadata;
    CHECK_FALSE(patch.optAlbum);
    CHECK_FALSE(patch.optYear);
    CHECK_FALSE(patch.optRecordingDate);
    controller.tryHandleEvent(applyEvent());
    CHECK_FALSE(controller.hasPendingSubmission());
    controller.tryHandleEvent(ftxui::Event::Escape);
    CHECK_FALSE(controller.isActive());
  }

  TEST_CASE("TrackEditController - an invalidated session goes stale and refuses to submit")
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

  TEST_CASE("TrackEditController - Ctrl-D does not stage a batch clear when the stale diagnostic hides its scope")
  {
    auto fixture = EditFixture{};
    auto const firstId = fixture.addTrack({.title = "First", .uri = "first.flac"});
    auto const secondId = fixture.addTrack({.title = "Second", .uri = "second.flac"});
    auto controller = fixture.makeController();
    REQUIRE(controller.tryOpen({firstId, secondId}));
    replaceField(controller, "Title", "Draft");

    fixture.commitUnrelatedChange();
    auto const* const editor = controller.activeEditor();
    REQUIRE(editor != nullptr);
    REQUIRE(editor->status() == TrackEditorStatus::Stale);
    CHECK_FALSE(renderElement(editor->render(), 80, 24).text.contains("Ctrl-D"));

    controller.tryHandleEvent(ftxui::Event::CtrlD);

    CHECK(editor->buildPatch().metadata.optTitle == "Draft");
    CHECK_FALSE(controller.hasPendingSubmission());
  }

  TEST_CASE("TrackEditController - reload rebinds a stale editor and resets its draft")
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

  TEST_CASE("TrackEditController - a submitted write outlives the editor it came from", "[concurrency]")
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

  TEST_CASE("TrackEditController - unified submission writes metadata and tags, deduplicating changed tracks")
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

  TEST_CASE("TrackEditController - quick tags reload preserves mode and writes only captured targets", "[concurrency]")
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

  TEST_CASE("TrackEditController credits - category preview stages one scope alongside tags and metadata")
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

  TEST_CASE("TrackEditController credits - focused entry retains stale child until confirmed reload")
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
