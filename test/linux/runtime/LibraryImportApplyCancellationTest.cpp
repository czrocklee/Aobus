// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/library/TrackTestSupport.h"
#include "test/runtime/AsyncTestSupport.h"
#include "test/runtime/ExecutorTestSupport.h"
#include "test/runtime/RuntimeLibraryTestSupport.h"
#include <ao/async/Runtime.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryTransfer.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <lmdb.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <memory>
#include <semaphore>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    enum class StopPoint : std::uint8_t
    {
      BindingRead,
      StagedRestore,
    };

    enum class BindingStopResult : std::uint8_t
    {
      NotReached,
      WriterFirst,
      BeforeMaintenance,
      WrongThread,
      ReadFailed,
      AlreadyStopped,
      Requested,
    };

    struct RestoreStopProbe final
    {
      StopPoint point;
      std::string_view databasePath;
      std::thread::id const callbackThread = std::this_thread::get_id();
      std::stop_source stopSource{};
      std::atomic_bool maintenanceDelivered{false};
      std::atomic<BindingStopResult> bindingStopResult{BindingStopResult::NotReached};
      std::atomic_bool staged{false};
      std::binary_semaphore resumeWriter{0};
    };

    std::atomic<RestoreStopProbe*>& armedProbe()
    {
      static constinit auto probe = std::atomic<RestoreStopProbe*>{nullptr};
      return probe;
    }

    RestoreStopProbe* claimProbe(StopPoint const point, ::MDB_env* env)
    {
      auto* probe = armedProbe().load();

      if (char const* nativePath = nullptr; probe == nullptr || probe->point != point ||
                                            ::mdb_env_get_path(env, &nativePath) != MDB_SUCCESS ||
                                            nativePath == nullptr || probe->databasePath != nativePath ||
                                            !armedProbe().compare_exchange_strong(probe, nullptr))
      {
        return nullptr;
      }

      return probe;
    }

    BindingStopResult requestStopDuringBinding(RestoreStopProbe& probe, unsigned int const flags, int const result)
    {
      if ((flags & MDB_RDONLY) == 0)
      {
        return BindingStopResult::WriterFirst;
      }

      if (!probe.maintenanceDelivered.load())
      {
        return BindingStopResult::BeforeMaintenance;
      }

      if (std::this_thread::get_id() == probe.callbackThread)
      {
        return BindingStopResult::WrongThread;
      }

      if (result != MDB_SUCCESS)
      {
        return BindingStopResult::ReadFailed;
      }

      return probe.stopSource.request_stop() ? BindingStopResult::Requested : BindingStopResult::AlreadyStopped;
    }
  } // namespace

  extern "C"
  {
    int nativeTxnBegin(::MDB_env* env,
                       ::MDB_txn* parent,
                       unsigned int flags,
                       ::MDB_txn** out) asm("__real_mdb_txn_begin");
    int probedTxnBegin(::MDB_env* env,
                       ::MDB_txn* parent,
                       unsigned int flags,
                       ::MDB_txn** out) asm("__wrap_mdb_txn_begin");
    int nativeDrop(::MDB_txn* transaction, ::MDB_dbi dbi, int del) asm("__real_mdb_drop");
    int gatedDrop(::MDB_txn* transaction, ::MDB_dbi dbi, int del) asm("__wrap_mdb_drop");

    int probedTxnBegin(::MDB_env* env, ::MDB_txn* parent, unsigned int const flags, ::MDB_txn** out)
    {
      // Claim the first matching top-level transaction on entry, so a writer also consumes the probe
      // and is reported rather than skipped.
      auto* const probe = parent == nullptr ? claimProbe(StopPoint::BindingRead, env) : nullptr;
      auto const result = nativeTxnBegin(env, parent, flags, out);

      if (probe != nullptr)
      {
        probe->bindingStopResult.store(requestStopDuringBinding(*probe, flags, result));
      }

      return result;
    }

    int gatedDrop(::MDB_txn* transaction, ::MDB_dbi dbi, int const del)
    {
      auto const result = nativeDrop(transaction, dbi, del);

      if (result == MDB_SUCCESS && del == 0)
      {
        if (auto* const probe = claimProbe(StopPoint::StagedRestore, ::mdb_txn_env(transaction));
            probe != nullptr && std::this_thread::get_id() != probe->callbackThread)
        {
          probe->staged.store(true);
          probe->resumeWriter.acquire();
        }
      }

      return result;
    }
  }

  // These stop schedules characterize current prepared YAML apply behavior. A deliberate change to
  // the cancellation policy should update the contract and these expectations together.
  TEST_CASE("LibraryJobs - caller stop after worker admission still publishes the completed restore", "[concurrency]")
  {
    auto const stopPoint = GENERATE(StopPoint::BindingRead, StopPoint::StagedRestore);
    CAPTURE(stopPoint);
    auto storageFixture = MusicLibraryFixture{};
    auto& storage = storageFixture.library();
    auto const existingId = library::test::addTrack(storage, {.title = "Existing", .uri = "existing.flac"});
    auto const path = storageFixture.root() / "restore.yaml";
    std::ofstream{path} << "version: 1\nexport-mode: full\nlibrary:\n"
                           "  resources: []\n  tracks:\n    - uri: restored.flac\n      title: Restored\n  lists: []\n";

    auto executor = QueuedExecutor{};
    auto changes = makeLibraryChanges(executor, storage);
    // The probe outlives the runtime, which joins the worker on unwinding.
    auto probe = RestoreStopProbe{.point = stopPoint, .databasePath = storage.databasePath().native()};
    auto runtimeFixture = LibraryCommandsFixture{storage, changes, executor};
    auto& runtime = runtimeFixture.runtime();
    auto const& library = runtimeFixture.library();
    auto& jobs = runtimeFixture.jobs();
    auto planRes = runQueuedTask(runtime, executor, jobs.prepareLibraryImportAsync(path, ImportMode::Restore));
    REQUIRE(planRes);
    auto const initialRevision = storage.libraryRevision(storage.readTransaction());
    auto published = std::vector<LibraryChangeSet>{};
    auto subscription =
      changes.onChanged([&published](LibraryChangeSet const& change) { published.push_back(change); });

    auto availabilitySubscription = library.onAuthoringAvailabilityChanged(
      [&probe](LibraryAuthoringAvailability const& availability)
      {
        if (availability.state == LibraryAuthoringState::Maintenance)
        {
          probe.maintenanceDelivered.store(true);
        }
      });
    auto completedPtr = std::make_shared<std::atomic_bool>(false);
    auto releaseProbe = utility::ScopedRegistration{[&probe]
                                                    {
                                                      armedProbe().store(nullptr);
                                                      probe.resumeWriter.release();
                                                    }};
    // In this fixture the first matching begin after arming is the target snapshot in
    // checkPreviewBindings. The wrapper cannot identify that call site: an earlier matching read
    // on the worker could also claim the probe. This case requests stop inline after worker
    // admission and before mutation submission; it does not assert which read was intercepted.
    armedProbe().store(&probe);
    auto future = runtime.spawn(flagCompletionAsync(
      completedPtr, jobs.applyLibraryImportPlanAsync(std::move(*planRes), probe.stopSource.get_token())));

    if (stopPoint == StopPoint::StagedRestore)
    {
      // The first table clear removes the seeded manifest row inside the write transaction,
      // before any tracks are rebuilt or the change is committed.
      REQUIRE(executor.tryDrainUntil([&] { return probe.staged.load() || completedPtr->load(); }));
      REQUIRE(probe.staged.load());
      REQUIRE(probe.stopSource.request_stop());
      {
        auto const read = storage.readTransaction();
        CHECK(storage.libraryRevision(read) == initialRevision);
        auto const manifest = storage.manifest().reader(read);
        auto const optExisting = manifest.get("existing.flac");
        REQUIRE(optExisting);
        CHECK(optExisting->trackId() == existingId);
        CHECK_FALSE(manifest.get("restored.flac"));
        CHECK(published.empty());
      }
      releaseProbe.reset();
    }

    REQUIRE(executor.tryDrainUntil([&] { return completedPtr->load(); }));

    if (stopPoint == StopPoint::BindingRead)
    {
      REQUIRE(probe.bindingStopResult.load() == BindingStopResult::Requested);
    }

    CHECK(probe.stopSource.stop_requested());
    auto const reportRes = future.get();
    REQUIRE(reportRes);
    CHECK(reportRes->tracksDeleted == 1);
    CHECK(reportRes->tracksCreated == 1);
    auto const expectedRevision = initialRevision + 1;
    CHECK(published == std::vector{LibraryChangeSet{.libraryRevision = expectedRevision, .libraryReset = true}});
    CHECK(library.authoringAvailability().state == LibraryAuthoringState::Available);

    auto const read = storage.readTransaction();
    CHECK(storage.libraryRevision(read) == expectedRevision);
    auto const manifest = storage.manifest().reader(read);
    CHECK_FALSE(manifest.get("existing.flac"));
    auto const optRestored = manifest.get("restored.flac");
    REQUIRE(optRestored);
    auto const tracks = storage.tracks().reader(read);
    CHECK_FALSE(tracks.get(existingId));
    auto const optTrack = tracks.get(optRestored->trackId());
    REQUIRE(optTrack);
    CHECK(optTrack->metadata().title() == "Restored");
  }
} // namespace ao::rt::test
