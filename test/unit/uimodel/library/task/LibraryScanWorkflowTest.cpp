// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/uimodel/library/task/LibraryScanWorkflow.h"

#include "test/unit/FilesystemTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/audio/AudioFixtureSupport.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/runtime/AsyncTestSupport.h"
#include "test/unit/runtime/ExecutorTestSupport.h"
#include "test/unit/runtime/RuntimeLibraryTestSupport.h"
#include <ao/async/OperationCancelled.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
#include <ao/utility/Path.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <gsl-lite/gsl-lite.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  namespace
  {
    struct LibraryScanWorkflowFixture final
    {
      rt::test::MusicLibraryFixture storage;
      rt::test::QueuedExecutor executor;
      rt::LibraryChanges changes{rt::test::makeLibraryChanges(executor, storage.library())};
      rt::test::LibraryCommandsFixture commandsFixture{storage.library(), changes, executor};
    };
  } // namespace

  TEST_CASE("LibraryScanWorkflow - plan decision distinguishes success, rejection, and work",
            "[uimodel][unit][library][scan]")
  {
    CHECK(decideLibraryScanPlan({}) == LibraryScanPlanDisposition::UpToDate);
    auto summary = LibraryScanPlanSummary{};
    summary.errorCount = 2;
    CHECK(decideLibraryScanPlan(summary) == LibraryScanPlanDisposition::ErrorsOnly);
    summary.newCount = 1;
    CHECK(decideLibraryScanPlan(summary) == LibraryScanPlanDisposition::Actionable);
    summary = {};
    summary.changedCount = 1;
    CHECK(decideLibraryScanPlan(summary) == LibraryScanPlanDisposition::Actionable);
    summary = {};
    summary.movedCount = 1;
    CHECK(decideLibraryScanPlan(summary) == LibraryScanPlanDisposition::Actionable);
    summary = {};
    summary.missingCount = 1;
    CHECK(decideLibraryScanPlan(summary) == LibraryScanPlanDisposition::Actionable);
  }

  TEST_CASE("LibraryScanWorkflow - mutation detection excludes issue-only results", "[uimodel][unit][library][scan]")
  {
    auto result = LibraryScanWorkflowResult{};
    CHECK_FALSE(result.hasMutatedLibrary());

    result.optApplyResult = rt::ScanApplyResult{};
    result.optApplyResult->missingCount = 1;
    CHECK_FALSE(result.hasMutatedLibrary());

    result.optApplyResult->insertedIds.emplace_back(1);
    CHECK(result.hasMutatedLibrary());
  }

  TEST_CASE("LibraryScanWorkflow - mutation detection observes each changed identity vector independently",
            "[uimodel][unit][library][scan]")
  {
    auto noApply = LibraryScanWorkflowResult{};
    CHECK_FALSE(noApply.hasMutatedLibrary());

    auto presentEmpty = LibraryScanWorkflowResult{};
    presentEmpty.optApplyResult.emplace();
    CHECK_FALSE(presentEmpty.hasMutatedLibrary());

    auto countOnly = LibraryScanWorkflowResult{};
    countOnly.optApplyResult.emplace();
    countOnly.optApplyResult->libraryRevision = 9;
    countOnly.optApplyResult->missingCount = 1;
    countOnly.optApplyResult->staleCount = 2;
    countOnly.optApplyResult->failureCount = 3;
    CHECK_FALSE(countOnly.hasMutatedLibrary());

    auto mutatedOnly = LibraryScanWorkflowResult{};
    mutatedOnly.optApplyResult.emplace();
    mutatedOnly.optApplyResult->mutatedIds.emplace_back(2);
    CHECK(mutatedOnly.hasMutatedLibrary());

    auto relinkedOnly = LibraryScanWorkflowResult{};
    relinkedOnly.optApplyResult.emplace();
    relinkedOnly.optApplyResult->relinkedIds.emplace_back(3);
    CHECK(relinkedOnly.hasMutatedLibrary());
  }

  TEST_CASE("LibraryScanWorkflow - an empty library completes as up to date without applying",
            "[uimodel][integration][library][scan]")
  {
    auto fixture = LibraryScanWorkflowFixture{};

    auto const res = rt::test::runQueuedTask(
      fixture.commandsFixture.runtime(),
      fixture.executor,
      runLibraryScanWorkflowAsync(&fixture.commandsFixture.library().jobs(), LibraryScanMode::Eager));

    REQUIRE(res);
    CHECK(res->disposition == LibraryScanPlanDisposition::UpToDate);
    CHECK(res->summary == LibraryScanPlanSummary{});
    CHECK(res->issues.empty());
    CHECK_FALSE(res->optApplyResult);
  }

  TEST_CASE("LibraryScanWorkflow - an errors-only plan returns exact issues without applying",
            "[uimodel][integration][library][scan]")
  {
    auto fixture = LibraryScanWorkflowFixture{};
    auto outside = ao::test::TempDir{};
    auto const outsideFile = outside.path() / "outside.flac";
    std::filesystem::copy_file(audio::test::requireAudioFixture("basic_metadata.flac"), outsideFile);
    auto const symlink =
      ao::test::SymlinkFixture{outsideFile, fixture.storage.root() / "alias.flac", ao::test::SymlinkType::File};

    auto const res = rt::test::runQueuedTask(
      fixture.commandsFixture.runtime(),
      fixture.executor,
      runLibraryScanWorkflowAsync(&fixture.commandsFixture.library().jobs(), LibraryScanMode::Eager));

    auto const expectedSummary = LibraryScanPlanSummary{.errorCount = 1};
    auto const expectedIssues = std::vector<LibraryScanIssue>{{
      .uri = "alias.flac",
      .message = "Library URI 'alias.flac' resolves outside the library root",
    }};
    REQUIRE(res);
    CHECK(res->disposition == LibraryScanPlanDisposition::ErrorsOnly);
    CHECK(res->summary == expectedSummary);
    CHECK(res->issues == expectedIssues);
    CHECK_FALSE(res->optApplyResult);
  }

  TEST_CASE("LibraryScanWorkflow - eager mode applies a new file with a known audio identity",
            "[uimodel][integration][library][scan]")
  {
    auto fixture = LibraryScanWorkflowFixture{};
    std::filesystem::copy_file(
      audio::test::requireAudioFixture("basic_metadata.flac"), fixture.storage.root() / "song.flac");

    auto const res = rt::test::runQueuedTask(
      fixture.commandsFixture.runtime(),
      fixture.executor,
      runLibraryScanWorkflowAsync(&fixture.commandsFixture.library().jobs(), LibraryScanMode::Eager));

    auto const expectedSummary = LibraryScanPlanSummary{.newCount = 1};
    REQUIRE(res);
    CHECK(res->disposition == LibraryScanPlanDisposition::Actionable);
    CHECK(res->summary == expectedSummary);
    CHECK(res->issues.empty());
    REQUIRE(res->optApplyResult);
    auto const& apply = *res->optApplyResult;
    REQUIRE(apply.insertedIds.size() == 1);
    CHECK(apply.mutatedIds.empty());
    CHECK(apply.relinkedIds.empty());
    CHECK(apply.missingCount == 0);
    CHECK(apply.staleCount == 0);
    CHECK(apply.failureCount == 0);
    CHECK(apply.libraryRevision == 1);
    CHECK(res->hasMutatedLibrary());
    CHECK_FALSE(res->shouldBackfillAudioIdentity);

    auto transaction = fixture.storage.library().readTransaction();
    auto const optManifest = fixture.storage.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == apply.insertedIds.front());
    CHECK(library::hasAudioIdentity(optManifest->audioPayloadLength(), optManifest->audioSignature()));
    CHECK(fixture.storage.library().libraryRevision(transaction) == apply.libraryRevision);
  }

  TEST_CASE("LibraryScanWorkflow - fast bootstrap applies a new file with pending audio identity",
            "[uimodel][integration][library][scan]")
  {
    auto fixture = LibraryScanWorkflowFixture{};
    std::filesystem::copy_file(
      audio::test::requireAudioFixture("basic_metadata.flac"), fixture.storage.root() / "song.flac");

    auto const res = rt::test::runQueuedTask(
      fixture.commandsFixture.runtime(),
      fixture.executor,
      runLibraryScanWorkflowAsync(&fixture.commandsFixture.library().jobs(), LibraryScanMode::FastBootstrap));

    auto const expectedSummary = LibraryScanPlanSummary{.newCount = 1};
    REQUIRE(res);
    CHECK(res->disposition == LibraryScanPlanDisposition::Actionable);
    CHECK(res->summary == expectedSummary);
    CHECK(res->issues.empty());
    REQUIRE(res->optApplyResult);
    auto const& apply = *res->optApplyResult;
    REQUIRE(apply.insertedIds.size() == 1);
    CHECK(apply.mutatedIds.empty());
    CHECK(apply.relinkedIds.empty());
    CHECK(apply.missingCount == 0);
    CHECK(apply.staleCount == 0);
    CHECK(apply.failureCount == 0);
    CHECK(apply.libraryRevision == 1);
    CHECK(res->hasMutatedLibrary());
    CHECK(res->shouldBackfillAudioIdentity);

    auto transaction = fixture.storage.library().readTransaction();
    auto const optManifest = fixture.storage.library().manifest().reader(transaction).get("song.flac");
    REQUIRE(optManifest);
    CHECK(optManifest->trackId() == apply.insertedIds.front());
    CHECK_FALSE(library::hasAudioIdentity(optManifest->audioPayloadLength(), optManifest->audioSignature()));
    CHECK(fixture.storage.library().libraryRevision(transaction) == apply.libraryRevision);
  }

  TEST_CASE("LibraryScanWorkflow - a missing root returns an exact planning failure",
            "[uimodel][integration][library][scan]")
  {
    auto temp = ao::test::TempDir{};
    auto const missingRoot = temp.path() / "missing-music";
    auto storage = library::test::makeTestMusicLibrary(missingRoot, temp.path() / "db");
    auto executor = rt::test::QueuedExecutor{};
    auto changes = rt::test::makeLibraryChanges(executor, storage);
    auto commandsFixture = rt::test::LibraryCommandsFixture{storage, changes, executor};

    auto const res =
      rt::test::runQueuedTask(commandsFixture.runtime(),
                              executor,
                              runLibraryScanWorkflowAsync(&commandsFixture.library().jobs(), LibraryScanMode::Eager));

    REQUIRE_FALSE(res);
    CHECK(res.error().stage == LibraryScanWorkflowStage::Planning);
    CHECK(res.error().error.code == Error::Code::NotFound);
    CHECK(res.error().error.message == "Library root path does not exist: " + utility::pathToUtf8(missingRoot));
    CHECK_FALSE(res.error().optPlanSummary);
  }

  TEST_CASE("LibraryScanWorkflow - cancellation before start reaches plan building without mutation",
            "[uimodel][integration][library][scan][concurrency]")
  {
    auto fixture = LibraryScanWorkflowFixture{};
    auto const withNewFile = GENERATE(false, true);
    INFO("Actionable input: " << withNewFile);

    if (withNewFile)
    {
      std::filesystem::copy_file(
        audio::test::requireAudioFixture("basic_metadata.flac"), fixture.storage.root() / "song.flac");
    }
    // The empty input would return before apply, so cancellation cannot be
    // observed only by forwarding the stopped token to the apply operation.
    auto stopSource = std::stop_source{};
    REQUIRE(stopSource.request_stop());

    CHECK_THROWS_AS(rt::test::runQueuedTask(
                      fixture.commandsFixture.runtime(),
                      fixture.executor,
                      runLibraryScanWorkflowAsync(
                        &fixture.commandsFixture.library().jobs(), LibraryScanMode::Eager, stopSource.get_token())),
                    async::OperationCancelled);

    auto transaction = fixture.storage.library().readTransaction();
    CHECK_FALSE(fixture.storage.library().manifest().reader(transaction).get("song.flac"));
    CHECK(fixture.storage.library().libraryRevision(transaction) == 0);
  }

  TEST_CASE("LibraryScanWorkflow - an occupied background lease returns an exact applying failure",
            "[uimodel][integration][library][scan][concurrency]")
  {
    auto applyRelease = rt::test::AsyncBarrier{};
    auto fixture = LibraryScanWorkflowFixture{};
    std::filesystem::copy_file(
      audio::test::requireAudioFixture("basic_metadata.flac"), fixture.storage.root() / "song.flac");
    auto& jobs = fixture.commandsFixture.library().jobs();
    auto activePlanRes = rt::LibraryScan{fixture.storage.library()}.buildPlan();
    REQUIRE(activePlanRes);
    auto applyEntered = rt::test::AsyncTestState<bool>::create(false);
    // Release before runtime teardown; keep the barrier alive until workers join.
    auto const release = gsl_lite::finally([&applyRelease] { applyRelease.release(); });
    auto activeCompletedPtr = std::make_shared<std::atomic_bool>(false);
    auto activeFuture = fixture.commandsFixture.runtime().spawn(
      rt::test::flagCompletionAsync(activeCompletedPtr,
                                    jobs.applyScanPlanAsync(std::move(*activePlanRes),
                                                            {},
                                                            {},
                                                            [applyEntered, &applyRelease](rt::ScanApplyProgress const&)
                                                            {
                                                              applyEntered.set(true);
                                                              applyRelease.wait();
                                                            })));

    REQUIRE(fixture.executor.tryDrainUntil([applyEntered] { return applyEntered.load(); }));

    auto const res = rt::test::runQueuedTask(
      fixture.commandsFixture.runtime(), fixture.executor, runLibraryScanWorkflowAsync(&jobs, LibraryScanMode::Eager));

    auto const expectedSummary = LibraryScanPlanSummary{.newCount = 1};
    REQUIRE_FALSE(res);
    CHECK(res.error().stage == LibraryScanWorkflowStage::Applying);
    CHECK(res.error().error.code == Error::Code::ResourceBusy);
    CHECK(res.error().error.message == "Another library background task is already active");
    REQUIRE(res.error().optPlanSummary);
    CHECK(*res.error().optPlanSummary == expectedSummary);

    applyRelease.release();
    REQUIRE(fixture.executor.tryDrainUntil([&activeCompletedPtr] { return activeCompletedPtr->load(); }));
    auto const activeApplyRes = activeFuture.get();
    REQUIRE(activeApplyRes);
    REQUIRE(activeApplyRes->insertedIds.size() == 1);
  }
} // namespace ao::uimodel::test
