// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/library/AudioFingerprintVerification.h"
#include "runtime/library/LibraryWriteLane.h"
#include "runtime/library/ScanApplyOperation.h"
#include "test/media/wav/TestWav.h"
#include "test/runtime/AsyncTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/library/AudioFingerprintReport.h>
#include <ao/rt/library/AudioIdentityIndex.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/ScopedRegistration.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <ios>
#include <memory>
#include <stop_token>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    constexpr auto kSongUri = "song.wav";
    constexpr auto kHandoffTimeout = std::chrono::seconds{10};

    void writeSong(std::filesystem::path const& path, std::uint8_t sampleByte, std::size_t payloadBytes = 1600)
    {
      auto const bytes =
        ao::test::wav::makeWav({.sampleRate = 8000, .audioData = std::vector<std::uint8_t>(payloadBytes, sampleByte)});
      auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
      output.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      output.close();
      REQUIRE(output);
    }

    template<typename Executor>
    struct FingerprintJobsFixture final
    {
      MusicLibraryFixture music;
      Executor executor;
      LibraryChanges changes = makeLibraryChanges(executor, music.library());
      LibraryCommandsFixture runtime{music.library(), changes, executor};

      std::filesystem::path songPath() const { return music.root() / kSongUri; }
      LibraryJobs& jobs() { return runtime.jobs(); }

      AudioFingerprintReport previewPending(std::size_t payloadBytes = 1600)
      {
        writeSong(songPath(), 1, payloadBytes);
        auto planRes = LibraryScan{music.library()}.buildPlan();
        REQUIRE(planRes);
        auto const importRes = runtime.runTask(jobs().applyScanPlanAsync(
          std::move(*planRes), ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew}));
        REQUIRE(importRes);
        REQUIRE(importRes->insertedIds.size() == 1);
        REQUIRE(importRes->failureCount == 0);
        auto previewRes = runtime.runTask(jobs().verifyAudioFingerprintAsync(kSongUri));
        REQUIRE(previewRes);
        REQUIRE(previewRes->status == AudioFingerprintStatus::Pending);
        REQUIRE(previewRes->optConfirmation);
        return std::move(*previewRes);
      }

      std::uint64_t revision() const { return music.library().libraryRevision(music.library().readTransaction()); }

      void checkPendingAt(std::uint64_t expectedRevision) const
      {
        auto const read = music.library().readTransaction();
        auto const optRow = music.library().manifest().reader(read).get(kSongUri);
        REQUIRE(optRow);
        CHECK(optRow->audioPayloadLength() == 0);
        CHECK(music.library().libraryRevision(read) == expectedRevision);
      }
    };
  } // namespace

  TEST_CASE("Audio fingerprint verification - a parked backfill excludes confirmation but permits preview",
            "[concurrency]")
  {
    auto releaseHash = AsyncBarrier{};
    auto fixture = FingerprintJobsFixture<QueuedExecutor>{};
    auto const preview = fixture.previewPending();
    auto const before = fixture.revision();
    auto publications = std::vector<LibraryChangeSet>{};
    auto const sub = fixture.changes.onChanged([&](LibraryChangeSet const& change) { publications.push_back(change); });
    auto stop = std::stop_source{};
    auto parked = AsyncTestState<bool>::create(false);
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto const callbackThread = std::this_thread::get_id();
    auto backfill = fixture.runtime.runtime().spawn(flagCompletionAsync(
      completedPtr,
      fixture.jobs().backfillAudioIdentityAsync(
        stop.get_token(),
        [parked, &releaseHash, callbackThread](AudioIdentityIndexProgress const& progress)
        {
          // Do not block the driving thread if worker affinity regresses.
          if (std::this_thread::get_id() != callbackThread && progress.itemFraction == 0.0 && !parked.load())
          {
            parked.set(true);
            releaseHash.wait();
          }
        })));
    auto releaseOnUnwind =
      utility::ScopedRegistration{[&]
                                  {
                                    std::ignore = stop.request_stop();
                                    releaseHash.release();
                                    AO_INVARIANT(fixture.executor.tryDrainUntil([&] { return completedPtr->load(); }),
                                                 "Parked backfill must settle before destroying the test fixture");
                                  }};
    REQUIRE(fixture.executor.tryDrainUntil([&] { return parked.load() || completedPtr->load(); }));
    REQUIRE(parked.load());

    auto const confirmRes =
      fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kSongUri, preview.optConfirmation));
    REQUIRE_FALSE(confirmRes);
    CHECK(confirmRes.error().code == Error::Code::ResourceBusy);

    auto const overlapRes = fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kSongUri));
    REQUIRE(overlapRes);
    CHECK(overlapRes->status == AudioFingerprintStatus::Pending);
    CHECK_FALSE(overlapRes->updated);
    CHECK_FALSE(overlapRes->optStored);
    CHECK(overlapRes->optConfirmation == preview.optConfirmation);
    CHECK(overlapRes->observed.signature == preview.observed.signature);
    CHECK(overlapRes->observed.payloadLength == preview.observed.payloadLength);
    CHECK_FALSE(completedPtr->load());
    fixture.checkPendingAt(before);
    CHECK(publications.empty());

    // Cancel before the first chunk so the backfill does not advance the revision.
    std::ignore = stop.request_stop();
    releaseHash.release();
    REQUIRE(fixture.executor.tryDrainUntil([&] { return completedPtr->load(); }));
    REQUIRE_THROWS_AS(std::ignore = backfill.get(), async::OperationCancelled);
    auto const retryRes =
      fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kSongUri, preview.optConfirmation));
    REQUIRE(retryRes);
    CHECK(retryRes->updated);
    CHECK(publications == std::vector{LibraryChangeSet{.libraryRevision = before + 1}});
  }

  TEST_CASE("Audio fingerprint verification - cancelling during payload inspection yields no observation",
            "[concurrency]")
  {
    auto fixture = FingerprintJobsFixture<QueuedExecutor>{};
    auto const preview = fixture.previewPending(5ULL * 1024 * 1024);
    auto const before = fixture.revision();
    auto stop = std::stop_source{};
    double lastFraction = 0.0;
    auto cancelAfterChunk = [&](double fraction)
    {
      lastFraction = fraction;

      if (fraction > 0.0 && fraction < 1.0)
      {
        std::ignore = stop.request_stop();
      }
    };
    REQUIRE_THROWS_AS(
      std::ignore = inspectAudioFingerprint(fixture.music.library(), kSongUri, stop.get_token(), cancelAfterChunk),
      async::OperationCancelled);
    CHECK(lastFraction > 0.0);
    CHECK(lastFraction < 1.0);
    fixture.checkPendingAt(before);

    auto hashedId = kInvalidLibraryTaskProgressId;
    std::size_t finishedCount = 0;
    bool finishedAfterHash = false;
    auto const sub = fixture.jobs().onProgress(
      [&](LibraryTaskProgressUpdated const& event)
      {
        if (event.kind == LibraryTaskProgressKind::Fingerprinting && event.subject == kSongUri && event.fraction == 1.0)
        {
          hashedId = event.id;
        }
      });
    auto const finishSub = fixture.jobs().onProgressFinished(
      [&](LibraryTaskProgressFinished const& event)
      {
        ++finishedCount;
        finishedAfterHash = event.id == hashedId;
      });
    auto const retryRes =
      fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kSongUri, preview.optConfirmation));
    REQUIRE(retryRes);
    CHECK(retryRes->updated);
    CHECK(finishedCount == 1);
    CHECK(finishedAfterHash);
  }

  TEST_CASE("Audio fingerprint verification - Closing stops confirmation during payload inspection", "[concurrency]")
  {
    auto music = MusicLibraryFixture{};
    auto& storage = music.library();
    writeSong(music.root() / kSongUri, 1, 5ULL * 1024 * 1024);
    auto planRes = LibraryScan{storage}.buildPlan();
    REQUIRE(planRes);
    auto const options = ScanApplyOptions{.audioIdentityPolicy = AudioIdentityPolicy::DeferNew};
    auto const importRes = ScanApplyOperation{storage, std::move(*planRes), {}, {}, options}.run();
    REQUIRE(importRes);
    REQUIRE(importRes->insertedIds.size() == 1);
    REQUIRE(importRes->failureCount == 0);
    auto const previewRes = inspectAudioFingerprint(storage, kSongUri, {});
    REQUIRE(previewRes);
    REQUIRE(previewRes->status == AudioFingerprintStatus::Pending);
    REQUIRE(previewRes->optConfirmation);

    auto const readManifest = [&] -> std::pair<std::uint64_t, std::vector<std::byte>>
    {
      auto const read = storage.readTransaction();
      auto const optRow = storage.manifest().reader(read).get(kSongUri);
      REQUIRE(optRow);
      auto const bytes = optRow->rawData();
      return {storage.libraryRevision(read), {bytes.begin(), bytes.end()}};
    };
    auto const [priorRevision, priorRow] = readManifest();
    auto executor = QueuedExecutor{};
    auto changes = makeLibraryChanges(executor, storage);
    std::size_t publicationCount = 0;
    auto const sub = changes.onChanged([&](LibraryChangeSet const&) { ++publicationCount; });
    auto writableRes = library::WritableMusicLibrary::acquire(storage);
    REQUIRE(writableRes);
    auto writeLanePtr = std::make_unique<LibraryWriteLane>(executor, std::move(*writableRes), changes);
    auto backgroundRes =
      writeLanePtr->beginBackgroundTask(LibraryWriteLane::BackgroundTaskKind::AudioFingerprintVerification);
    REQUIRE(backgroundRes);

    auto const parked = AsyncTestState<bool>::create(false);
    auto const releaseHash = AsyncTestState<bool>::create(false);
    bool releasedByStop = false;
    double lastFraction = 0.0;
    auto inspect = [&](std::stop_token effectiveToken)
    {
      auto afterChunk = [&](double fraction)
      {
        lastFraction = fraction;

        if (fraction > 0.0 && fraction < 1.0 && !parked.load())
        {
          auto releaseOnStop = std::stop_callback{effectiveToken, [&] { releaseHash.set(true); }};
          parked.set(true);
          // Bound teardown even if Closing no longer reaches the inspection.
          releasedByStop = releaseHash.tryWaitUntil(true, kHandoffTimeout);
        }
      };
      return inspectAudioFingerprint(storage, kSongUri, effectiveToken, afterChunk);
    };
    auto worker = boost::asio::thread_pool{1};
    auto future = boost::asio::co_spawn(worker,
                                        confirmAudioFingerprintAsync(writeLanePtr->captureSubmission(),
                                                                     &*backgroundRes,
                                                                     &storage,
                                                                     *previewRes->optConfirmation,
                                                                     {},
                                                                     std::move(inspect)),
                                        boost::asio::use_future);
    auto const entered = parked.tryWaitUntil(true, kHandoffTimeout);

    // Close on the owner, then drain and join even if the hash did not park.
    writeLanePtr.reset();
    auto const settled = executor.tryDrainUntil(
      [&] { return future.wait_for(std::chrono::seconds{0}) == std::future_status::ready; }, kHandoffTimeout);

    if (!settled)
    {
      worker.stop();
    }

    worker.join();
    executor.drain();

    REQUIRE(entered);
    REQUIRE(settled);
    CHECK(releasedByStop);
    // A post-hash stop would still cancel the future without writing.
    CHECK(lastFraction > 0.0);
    CHECK(lastFraction < 1.0);
    CHECK_THROWS_AS(std::ignore = future.get(), async::OperationCancelled);
    auto const [revision, row] = readManifest();
    CHECK(revision == priorRevision);
    CHECK(row == priorRow);
    CHECK(publicationCount == 0);
    auto const freshWritableRes = library::WritableMusicLibrary::acquire(storage);
    CHECK(freshWritableRes);
  }

  TEST_CASE("Audio fingerprint verification - a queued confirmation rechecks evidence and caller stop at grant",
            "[concurrency]")
  {
    // The contexts outlive the fixture, including any worker hop on failure.
    auto interactiveContext = boost::asio::io_context{};
    auto confirmContext = boost::asio::io_context{};
    auto fixture = FingerprintJobsFixture<ManualExecutor>{};
    auto const preview = fixture.previewPending();
    auto const before = fixture.revision();
    auto publications = std::vector<LibraryChangeSet>{};
    auto const sub = fixture.changes.onChanged([&](LibraryChangeSet const& change) { publications.push_back(change); });
    std::size_t finishedCount = 0;
    auto const finishSub =
      fixture.jobs().onProgressFinished([&](LibraryTaskProgressFinished const&) { ++finishedCount; });
    auto stop = std::stop_source{};
    auto const poll = [](boost::asio::io_context& context)
    {
      context.restart();
      std::ignore = context.poll();
    };
    auto interactive = std::future<Result<>>{};
    auto confirm = std::future<Result<AudioFingerprintReport>>{};
    auto const drain = [&]
    {
      auto const settled = [](auto const& task)
      { return !task.valid() || task.wait_for(std::chrono::seconds{0}) == std::future_status::ready; };
      return fixture.executor.tryDrainUntil(
        [&]
        {
          poll(interactiveContext);
          poll(confirmContext);
          return settled(interactive) && settled(confirm);
        });
    };
    auto releaseOnUnwind =
      utility::ScopedRegistration{[&]
                                  {
                                    // Retire queued grants and drain the final callback before
                                    // fixture teardown, even on assertion failure.
                                    std::ignore = stop.request_stop();
                                    AO_INVARIANT(drain(), "Queued library jobs must settle before test teardown");
                                  }};

    // Leave the interactive command's grant continuation unpumped. Its dry
    // run does not advance the revision, so it cannot mask stale audio.
    interactive = boost::asio::co_spawn(interactiveContext,
                                        fixture.runtime.commands().previewCreateListAsync(ListDraft{.name = "Preview"}),
                                        boost::asio::use_future);
    REQUIRE(interactiveContext.poll_one() == 1);
    REQUIRE(fixture.executor.queuedCount() == 0);

    confirm = boost::asio::co_spawn(
      confirmContext,
      fixture.jobs().verifyAudioFingerprintAsync(kSongUri, preview.optConfirmation, stop.get_token()),
      boost::asio::use_future);
    REQUIRE(confirmContext.poll_one() == 1);
    REQUIRE(fixture.executor.tryRunOne());
    // The worker hop posts back to this coroutine's associated Asio context.
    // Run that continuation through enqueue, but leave the interactive grant parked.
    REQUIRE(confirmContext.run_one_for(kHandoffTimeout) == 1);
    REQUIRE(confirmContext.poll_one() == 0);

    auto const settle = [&]
    {
      REQUIRE(drain()); // The interactive abort releases the queued grant.

      REQUIRE(interactive.get());
      CHECK(finishedCount == 1);
      fixture.checkPendingAt(before);
      CHECK(publications.empty());
    };

    SECTION("different audio at the same size and mtime invalidates the token")
    {
      auto const path = fixture.songPath();
      auto const size = std::filesystem::file_size(path);
      auto const writeTime = std::filesystem::last_write_time(path);
      writeSong(path, 2);
      std::filesystem::last_write_time(path, writeTime);
      REQUIRE(std::filesystem::file_size(path) == size);
      REQUIRE((std::filesystem::last_write_time(path) == writeTime));

      settle();
      auto const staleRes = confirm.get();
      REQUIRE_FALSE(staleRes);
      CHECK(staleRes.error().code == Error::Code::Conflict);
      auto const freshRes = fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kSongUri));
      REQUIRE(freshRes);
      CHECK(freshRes->observed.signature != preview.observed.signature);
      REQUIRE(freshRes->optConfirmation);
      CHECK(freshRes->optConfirmation != preview.optConfirmation);
    }

    SECTION("caller stop leaves the same token usable after the queue drains")
    {
      std::ignore = stop.request_stop();
      settle();
      REQUIRE_THROWS_AS(std::ignore = confirm.get(), async::OperationCancelled);

      auto const retryRes =
        fixture.runtime.runTask(fixture.jobs().verifyAudioFingerprintAsync(kSongUri, preview.optConfirmation));
      REQUIRE(retryRes);
      CHECK(retryRes->updated);
      CHECK(publications == std::vector{LibraryChangeSet{.libraryRevision = before + 1}});
    }
  }
} // namespace ao::rt::test
