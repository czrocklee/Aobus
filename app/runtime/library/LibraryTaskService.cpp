// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryTaskService.h>

#include "AudioIdentityBatchWriter.h"
#include "LibraryMutationService.h"
#include "LibraryYamlImportOperation.h"
#include "ResourceCarrierIndex.h"
#include "ResourceMaterialization.h"
#include "ScanApplyOperation.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/compat/AtomicSharedPtr.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ReadTransaction.h>
#include <ao/library/ResourceLayout.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/AudioIdentityIndex.h>
#include <ao/rt/library/AudioIdentityIndexer.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/rt/resource/ResourceDiskCache.h>
#include <ao/utility/Path.h>
#include <ao/utility/ThreadName.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  struct LibraryImportPlan::Impl final
  {
    Impl(LibraryYamlImportOperation::PreparedImport preparedValue,
         ImportReport reportValue,
         std::array<std::byte, 16> targetLibraryIdValue,
         std::uint64_t runtimeInstanceIdValue,
         std::uint64_t targetRevisionValue)
      : prepared{std::move(preparedValue)}
      , report{reportValue}
      , targetLibraryId{targetLibraryIdValue}
      , runtimeInstanceId{runtimeInstanceIdValue}
      , targetRevision{targetRevisionValue}
    {
    }

    LibraryYamlImportOperation::PreparedImport prepared;
    ImportReport report;
    std::array<std::byte, 16> targetLibraryId{};
    std::uint64_t runtimeInstanceId = 0;
    std::uint64_t targetRevision = 0;
  };

  LibraryImportPlan::LibraryImportPlan(std::unique_ptr<Impl> implPtr)
    : _implPtr{std::move(implPtr)}
  {
  }

  LibraryImportPlan::~LibraryImportPlan() = default;
  LibraryImportPlan::LibraryImportPlan(LibraryImportPlan&&) noexcept = default;
  LibraryImportPlan& LibraryImportPlan::operator=(LibraryImportPlan&&) noexcept = default;

  ImportReport const& LibraryImportPlan::report() const noexcept
  {
    AO_EXPECTS(_implPtr != nullptr);
    return _implPtr->report;
  }

  namespace
  {
    using LibraryTaskProgressPublisher =
      compat::MoveOnlyFunction<void(LibraryTaskProgressKind kind, double fraction, std::string subject)>;

    LibraryTaskProgressKind scanApplyProgressKind(ScanApplyProgress const& progress)
    {
      if (progress.stage == ScanApplyProgressStage::Fingerprinting)
      {
        return LibraryTaskProgressKind::Fingerprinting;
      }

      return LibraryTaskProgressKind::Updating;
    }

    bool shouldPublishBackfillProgress(AudioIdentityIndexProgress const& progress)
    {
      constexpr std::int32_t kBackfillProgressItemInterval = 25;
      return progress.itemFraction == 0.0 && progress.processedCount % kBackfillProgressItemInterval == 0;
    }

    double backfillProgressFraction(AudioIdentityIndexProgress const& progress)
    {
      if (progress.totalCount <= 0)
      {
        return 0.0;
      }

      return std::min(1.0, static_cast<double>(progress.processedCount) / static_cast<double>(progress.totalCount));
    }

    struct CoordinatedScanResult final
    {
      ScanApplyResult result;
      bool cancelled = false;
    };

    Result<CoordinatedScanResult> applyCoordinatedScan(
      LibraryMutationService& mutationService,
      LibraryMutationService::BackgroundTaskLease const& backgroundTask,
      library::MusicLibrary& library,
      ScanPlan plan,
      ScanApplyOptions options,
      compat::MoveOnlyFunction<void(ScanApplyProgress const&)> progress,
      compat::MoveOnlyFunction<void(ScanFailure const&)> failure,
      std::stop_token stopToken)
    {
      auto operation = ScanApplyOperation{library, std::move(plan), std::move(progress), std::move(failure), options};
      auto prepareRes = operation.prepare(stopToken);

      if (!prepareRes)
      {
        return std::unexpected{prepareRes.error()};
      }

      if (operation.cancelled())
      {
        return CoordinatedScanResult{.result = std::move(*prepareRes), .cancelled = true};
      }

      auto revalidationRes = operation.revalidatePreparedFiles(stopToken);

      if (!revalidationRes)
      {
        return std::unexpected{revalidationRes.error()};
      }

      if (operation.cancelled())
      {
        return CoordinatedScanResult{.result = std::move(*revalidationRes), .cancelled = true};
      }

      if (!operation.readyForMutation())
      {
        return CoordinatedScanResult{.result = std::move(*revalidationRes)};
      }

      auto mutationRes = mutationService.beginBackgroundMutation(backgroundTask);

      if (!mutationRes)
      {
        return std::unexpected{mutationRes.error()};
      }

      auto mutation = std::move(*mutationRes);
      auto executionRes = mutation.execute(
        [&operation, stopToken](library::LibraryWrite& transaction) -> Result<OperationOutcome<CoordinatedScanResult>>
        {
          auto applyRes = operation.apply(transaction, stopToken);

          if (!applyRes)
          {
            return std::unexpected{applyRes.error()};
          }

          auto result = std::move(*applyRes);

          if (operation.cancelled())
          {
            return Unchanged<CoordinatedScanResult>{
              .value = CoordinatedScanResult{.result = std::move(result), .cancelled = true},
            };
          }

          if (!operation.transactionShouldCommit())
          {
            return Unchanged<CoordinatedScanResult>{
              .value = CoordinatedScanResult{.result = std::move(result)},
            };
          }

          auto mutatedIds = result.mutatedIds;
          mutatedIds.append_range(result.relinkedIds);
          auto changeSet = LibraryChangeSet{
            .tracksInserted = result.insertedIds,
            .tracksMutated = std::move(mutatedIds),
          };
          return Changed<CoordinatedScanResult>{
            .value = CoordinatedScanResult{.result = std::move(result)},
            .changeSet = std::move(changeSet),
          };
        },
        "Apply scan plan");

      if (!executionRes)
      {
        return std::unexpected{executionRes.error()};
      }

      auto coordinated = std::move(executionRes->value);

      if (executionRes->optCommittedRevision)
      {
        coordinated.result.libraryRevision = *executionRes->optCommittedRevision;
      }

      return coordinated;
    }

    void logLibraryTaskFailure(std::string_view stage, std::string_view uri, std::string_view message)
    {
      if (uri.empty())
      {
        APP_LOG_ERROR("Failed to {}: {}", stage, message);
        return;
      }

      APP_LOG_ERROR("Failed to {} {}: {}", stage, uri, message);
    }

    LibraryTaskService::ScanProgressCallback makeScanProgressReporter(std::size_t totalItems,
                                                                      LibraryTaskProgressPublisher publish,
                                                                      LibraryTaskService::ScanProgressCallback callback)
    {
      return [totalItems, publish = std::move(publish), callback = std::move(callback)](
               ScanApplyProgress const& progress) mutable
      {
        auto const itemBase = static_cast<double>(progress.itemIndex);
        auto const fraction =
          totalItems > 0 ? (itemBase + progress.itemFraction) / static_cast<double>(totalItems) : 0.0;
        publish(scanApplyProgressKind(progress), fraction, utility::pathToUtf8(progress.path.filename()));

        if (callback)
        {
          callback(progress);
        }
      };
    }

    LibraryTaskService::ScanFailureCallback makeScanFailureReporter(LibraryTaskService::ScanFailureCallback callback)
    {
      return [callback = std::move(callback)](ScanFailure const& failure) mutable
      {
        // Diagnostics run on the worker thread; spdlog is thread-safe and the
        // failure's string views are only valid for the duration of this call.
        logLibraryTaskFailure(failure.stage, failure.uri, failure.message);

        if (callback)
        {
          callback(failure);
        }
      };
    }

    AudioIdentityIndexer::CommitBatchCallback makeAudioIdentityCommitBatch(
      LibraryMutationService& mutationService,
      LibraryMutationService::BackgroundTaskLease const& backgroundTask)
    {
      return [mutationServiceRaw = &mutationService, backgroundTaskRaw = &backgroundTask](
               std::span<AudioIdentityWriteCandidate const> candidates) -> Result<AudioIdentityBatchCommitResult>
      {
        auto mutationRes = mutationServiceRaw->beginBackgroundMutation(*backgroundTaskRaw);

        if (!mutationRes)
        {
          return std::unexpected{mutationRes.error()};
        }

        auto mutation = std::move(*mutationRes);
        auto executionRes = mutation.execute(
          [candidates](library::LibraryWrite& transaction) -> Result<OperationOutcome<AudioIdentityBatchCommitResult>>
          {
            auto result = applyAudioIdentityBatch(transaction, candidates);

            if (!result)
            {
              return std::unexpected{result.error()};
            }

            if (result->completedCount == 0)
            {
              return Unchanged<AudioIdentityBatchCommitResult>{.value = std::move(*result)};
            }

            return Changed<AudioIdentityBatchCommitResult>{.value = std::move(*result), .changeSet = {}};
          },
          "Backfill audio identity");

        if (!executionRes)
        {
          return std::unexpected{executionRes.error()};
        }

        return std::move(executionRes->value);
      };
    }

    AudioIdentityIndexProgressCallback makeAudioIdentityProgressReporter(LibraryTaskProgressPublisher publish,
                                                                         AudioIdentityIndexProgressCallback callback)
    {
      return [publish = std::move(publish),
              callback = std::move(callback)](AudioIdentityIndexProgress const& progress) mutable
      {
        if (shouldPublishBackfillProgress(progress))
        {
          auto const fraction = backfillProgressFraction(progress);
          publish(
            LibraryTaskProgressKind::IndexingAudioIdentity, fraction, utility::pathToUtf8(progress.path.filename()));
        }

        if (callback)
        {
          callback(progress);
        }
      };
    }

    AudioIdentityIndexFailureCallback makeAudioIdentityFailureReporter(AudioIdentityIndexFailureCallback callback)
    {
      return [callback = std::move(callback)](AudioIdentityIndexFailure const& failure) mutable
      {
        logLibraryTaskFailure(failure.stage, failure.uri, failure.message);

        if (callback)
        {
          callback(failure);
        }
      };
    }
  } // namespace

  struct LibraryTaskService::Impl final
  {
    struct Signals final
    {
      async::Signal<> progressFinished;
      async::Signal<LibraryTaskProgressUpdated const&> progress;
    };

    struct ProgressDeliveryState final
    {
      ProgressDeliveryState(async::Executor& executor, std::weak_ptr<Signals> weakSignalsPtr)
        : executorRaw{&executor}, weakSignalsPtr{std::move(weakSignalsPtr)}
      {
      }

      async::Executor* executorRaw = nullptr;
      std::weak_ptr<Signals> weakSignalsPtr;
      std::mutex mutex;
      std::vector<LibraryTaskProgressUpdated> pendingEvents;
      bool deliveryPending = false;
    };

    Impl(async::Runtime& runtimeRef,
         library::MusicLibrary& libraryRef,
         LibraryMutationService& mutationServiceRef,
         std::filesystem::path cacheDirectory)
      : asyncRuntime{runtimeRef}
      , library{libraryRef}
      , mutationService{mutationServiceRef}
      , diskCache{ResourceDiskCache::Config{.directory = coverCacheDirectory(cacheDirectory),
                                            .maximumEntryBytes = kMaximumInteractiveResourceBytes}}
    {
    }

    /**
     * @brief The carrier index, rebuilt when the one on hand is behind.
     *
     * The whole sequence — re-check the slot, open a read transaction, build,
     * publish — happens under one mutex, and that is what makes the published
     * stamp monotonic without comparing anything. A read transaction pins the
     * revision it begins at, so a builder that opened its transaction before
     * taking the lock could finish a revision-N snapshot after another builder
     * published N+1; opening it inside the critical section removes that
     * interleaving rather than detecting it.
     *
     * A snapshot at least as new as @p requestRevision needs no rebuild, which is
     * why a burst arriving on one stale stamp costs one build: the first builder
     * publishes and the rest re-check and find it.
     */
    std::shared_ptr<ResourceCarrierIndex const> rebuildCarrierIndex(std::uint64_t const requestRevision)
    {
      auto const lock = std::scoped_lock{carrierIndexMutex};

      if (auto const currentPtr = carrierIndexSlot.load(); currentPtr && currentPtr->answersRevision(requestRevision))
      {
        return currentPtr;
      }

      auto const transaction = library.readTransaction();
      auto snapshotPtr = std::make_shared<ResourceCarrierIndex const>(buildResourceCarrierIndex(library, transaction));
      carrierIndexBuildCount.fetch_add(1);
      carrierIndexSlot.store(snapshotPtr);
      return snapshotPtr;
    }

    /**
     * @brief Resolves @p resourceId to bytes, on a worker.
     *
     * The read transaction covers the descriptor, the revision, and a load of the
     * snapshot slot, and closes before any cache or file I/O: a long-lived read
     * snapshot holds back page reuse for every concurrent writer.
     */
    Result<std::optional<std::vector<std::byte>>> loadResource(ResourceId const resourceId,
                                                               ResourceSizeLimit const limit,
                                                               std::stop_token const& stopToken)
    {
      auto optDescriptor = std::optional<library::ResourceDescriptor>{};
      std::uint64_t revision = 0;
      auto indexPtr = std::shared_ptr<ResourceCarrierIndex const>{};

      {
        auto const transaction = library.readTransaction();
        optDescriptor = library.resources().reader(transaction).get(resourceId);
        revision = library.libraryRevision(transaction);
        indexPtr = carrierIndexSlot.load();
      }

      if (!optDescriptor)
      {
        return std::optional<std::vector<std::byte>>{};
      }

      if (!indexPtr || !indexPtr->answersRevision(revision))
      {
        // A request holding a usable snapshot never reaches the mutex; this one
        // does, and may find that another worker has already published.
        indexPtr = rebuildCarrierIndex(revision);
      }

      auto const context = ResourceMaterializationContext{
        .descriptor = *optDescriptor,
        .candidateUris = indexPtr->carrierUris(resourceId),
        .musicRoot = library.rootPath(),
        .cache = diskCache,
        .optMaximumBytes =
          limit == ResourceSizeLimit::Interactive ? std::optional{kMaximumInteractiveResourceBytes} : std::nullopt,
      };
      return materializeResource(context, stopToken);
    }

    LibraryTaskProgressPublisher makeProgressPublisher()
    {
      auto statePtr = std::make_shared<ProgressDeliveryState>(asyncRuntime.callbackExecutor(), signalsPtr);

      return [statePtr](LibraryTaskProgressKind kind, double fraction, std::string subject)
      {
        bool scheduleDelivery = false;

        {
          auto const lock = std::scoped_lock{statePtr->mutex};
          auto event = LibraryTaskProgressUpdated{
            .kind = kind,
            .fraction = fraction,
            .subject = std::move(subject),
          };

          if (!statePtr->pendingEvents.empty() && statePtr->pendingEvents.back().kind == kind)
          {
            statePtr->pendingEvents.back() = std::move(event);
          }
          else
          {
            statePtr->pendingEvents.push_back(std::move(event));
          }

          scheduleDelivery = !statePtr->deliveryPending;
          statePtr->deliveryPending = true;
        }

        if (!scheduleDelivery)
        {
          return;
        }

        try
        {
          statePtr->executorRaw->dispatch(
            [statePtr]
            {
              auto events = std::vector<LibraryTaskProgressUpdated>{};

              {
                auto const lock = std::scoped_lock{statePtr->mutex};
                events = std::move(statePtr->pendingEvents);
                statePtr->pendingEvents.clear();
                statePtr->deliveryPending = false;
              }

              if (auto const signalsPtr = statePtr->weakSignalsPtr.lock(); signalsPtr != nullptr)
              {
                for (auto const& event : events)
                {
                  signalsPtr->progress.emit(event);
                }
              }
            });
        }
        catch (...)
        {
          {
            auto const lock = std::scoped_lock{statePtr->mutex};
            statePtr->pendingEvents.clear();
            statePtr->deliveryPending = false;
          }

          throw;
        }
      };
    }

    void notifyProgressFinished() const noexcept { signalsPtr->progressFinished.emit(); }

    async::Runtime& asyncRuntime;
    library::MusicLibrary& library;
    LibraryMutationService& mutationService;
    ResourceDiskCache diskCache;
    std::shared_ptr<Signals> signalsPtr = std::make_shared<Signals>();

    /// An immutable snapshot makes its contents safe to share; the slot holding
    /// it is a separate object and needs its own rule, so it is atomic. A request
    /// loads it once and then needs no synchronization at all, because the graph
    /// behind its own copy cannot change.
    compat::AtomicSharedPtr<ResourceCarrierIndex const> carrierIndexSlot{};
    std::mutex carrierIndexMutex;
    std::atomic<std::uint64_t> carrierIndexBuildCount{0};
  };

  LibraryTaskService::LibraryTaskService(async::Runtime& asyncRuntime,
                                         library::MusicLibrary& library,
                                         LibraryMutationService& mutationService,
                                         std::filesystem::path cacheDirectory)
    : _implPtr{std::make_unique<Impl>(asyncRuntime, library, mutationService, std::move(cacheDirectory))}
  {
  }

  LibraryTaskService::~LibraryTaskService() = default;

  async::Subscription LibraryTaskService::onProgressFinished(compat::MoveOnlyFunction<void()> handler) const
  {
    return _implPtr->signalsPtr->progressFinished.connect(std::move(handler));
  }

  async::Subscription LibraryTaskService::onProgress(
    compat::MoveOnlyFunction<void(LibraryTaskProgressUpdated const&)> handler) const
  {
    return _implPtr->signalsPtr->progress.connect(std::move(handler));
  }

  std::uint64_t LibraryTaskService::resourceCarrierIndexBuildCount() const noexcept
  {
    return _implPtr->carrierIndexBuildCount.load();
  }

  async::Task<Result<std::optional<std::vector<std::byte>>>> LibraryTaskService::loadResourceAsync(
    ResourceId const resourceId,
    ResourceSizeLimit const limit,
    std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);

    if (resourceId == kInvalidResourceId)
    {
      co_return std::optional<std::vector<std::byte>>{};
    }

    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    auto result = _implPtr->loadResource(resourceId, limit, stopToken);
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
    co_return result;
  }

  async::Task<Result<LibraryImportPlan>> LibraryTaskService::prepareLibraryImportAsync(std::filesystem::path path,
                                                                                       ImportMode const mode,
                                                                                       std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
    auto backgroundTaskRes =
      _implPtr->mutationService.beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::Import);

    if (!backgroundTaskRes)
    {
      co_return std::unexpected{backgroundTaskRes.error()};
    }

    auto backgroundTask = std::move(*backgroundTaskRes);
    auto maintenanceRes = _implPtr->mutationService.beginMaintenance(LibraryMaintenanceKind::Import);

    if (!maintenanceRes)
    {
      co_return std::unexpected{maintenanceRes.error()};
    }

    auto maintenance = std::move(*maintenanceRes);
    auto const availability = _implPtr->mutationService.availability();
    auto optRes = std::optional<Result<LibraryImportPlan>>{};
    auto exceptionPtr = std::exception_ptr{};
    bool cancelledByException = false;

    try
    {
      co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
      setCurrentThreadName("LibraryImportPreview");
      optRes.emplace(
        [&] -> Result<LibraryImportPlan>
        {
          auto importer = ao::rt::LibraryYamlImporter{_implPtr->library};
          auto importOperation = LibraryYamlImportOperation{importer};
          auto preparedRes = importOperation.prepare(path, mode, true);

          if (!preparedRes)
          {
            return std::unexpected{preparedRes.error()};
          }

          auto targetLibraryId = std::array<std::byte, 16>{};
          std::uint64_t targetRevision = 0;

          {
            auto readTransaction = _implPtr->library.readTransaction();
            targetLibraryId = _implPtr->library.metadataHeader(readTransaction).libraryId;
            targetRevision = _implPtr->library.libraryRevision(readTransaction);
          }

          auto mutationRes = _implPtr->mutationService.beginMaintenanceMutation(maintenance);

          if (!mutationRes)
          {
            return std::unexpected{mutationRes.error()};
          }

          auto mutation = std::move(*mutationRes);
          auto reportRes = mutation.apply([&importOperation, &preparedRes](library::LibraryWrite& transaction)
                                          { return importOperation.preview(*preparedRes, transaction); });

          if (!reportRes)
          {
            return std::unexpected{reportRes.error()};
          }

          auto report = std::move(*reportRes);
          mutation.abort();
          return LibraryImportPlan{std::make_unique<LibraryImportPlan::Impl>(
            std::move(*preparedRes), report, targetLibraryId, availability.runtimeInstanceId, targetRevision)};
        }());
    }
    catch (std::exception const& error)
    {
      if (async::isOperationCancelled(error))
      {
        cancelledByException = true;
      }
      else
      {
        exceptionPtr = std::current_exception();
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      exceptionPtr = std::current_exception();
    }

    try
    {
      co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();
    }
    // NOLINTNEXTLINE(aobus-async-cancellation-guard): This no-stop admission treats every live rejection as fatal.
    catch (...)
    {
      _implPtr->mutationService.handleFinalizationAdmissionFailure(std::current_exception());
      async::throwOperationCancelled();
    }

    maintenance.finish();
    backgroundTask.finish();

    if (cancelledByException)
    {
      async::throwOperationCancelled();
    }

    if (exceptionPtr)
    {
      std::rethrow_exception(exceptionPtr);
    }

    if (stopToken.stop_requested())
    {
      async::throwOperationCancelled();
    }

    AO_INVARIANT(optRes);
    co_return std::move(*optRes);
  }

  async::Task<Result<ImportReport>> LibraryTaskService::applyLibraryImportPlanAsync(LibraryImportPlan plan,
                                                                                    std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);

    AO_EXPECTS(plan._implPtr, "Import plan has already been consumed");

    auto backgroundTaskRes =
      _implPtr->mutationService.beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::Import);

    if (!backgroundTaskRes)
    {
      co_return std::unexpected{backgroundTaskRes.error()};
    }

    auto backgroundTask = std::move(*backgroundTaskRes);
    auto maintenanceRes = _implPtr->mutationService.beginMaintenance(LibraryMaintenanceKind::Import);

    if (!maintenanceRes)
    {
      co_return std::unexpected{maintenanceRes.error()};
    }

    auto maintenance = std::move(*maintenanceRes);
    auto const availability = _implPtr->mutationService.availability();
    auto optRes = std::optional<Result<ImportReport>>{};
    auto exceptionPtr = std::exception_ptr{};
    bool cancelledByException = false;

    try
    {
      co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
      setCurrentThreadName("LibraryImport");
      optRes.emplace(
        [&] -> Result<ImportReport>
        {
          auto const& binding = *plan._implPtr;

          if (availability.runtimeInstanceId != binding.runtimeInstanceId)
          {
            return makeError(Error::Code::Conflict, "Import plan belongs to a different library runtime");
          }

          {
            auto readTransaction = _implPtr->library.readTransaction();
            auto const header = _implPtr->library.metadataHeader(readTransaction);

            if (header.libraryId != binding.targetLibraryId ||
                _implPtr->library.libraryRevision(readTransaction) != binding.targetRevision)
            {
              return makeError(Error::Code::Conflict, "Target library changed after the import preview");
            }
          }

          auto importer = ao::rt::LibraryYamlImporter{_implPtr->library};
          auto importOperation = LibraryYamlImportOperation{importer};

          if (auto sourceRes = importOperation.revalidateSource(binding.prepared); !sourceRes)
          {
            return std::unexpected{sourceRes.error()};
          }

          auto mutationRes = _implPtr->mutationService.beginMaintenanceMutation(maintenance);

          if (!mutationRes)
          {
            return std::unexpected{mutationRes.error()};
          }

          auto mutation = std::move(*mutationRes);
          auto executionRes = mutation.execute(
            [&importOperation, &binding](library::LibraryWrite& transaction) -> Result<OperationOutcome<ImportReport>>
            {
              auto importRes = importOperation.apply(binding.prepared, transaction);

              if (!importRes)
              {
                return std::unexpected{importRes.error()};
              }

              auto changeSet = importOperation.buildChangeSet(binding.prepared, transaction);
              return Changed<ImportReport>{
                .value = std::move(*importRes),
                .changeSet = std::move(changeSet),
              };
            },
            "Import library YAML");

          if (!executionRes)
          {
            return std::unexpected{executionRes.error()};
          }

          return std::move(executionRes->value);
        }());
    }
    catch (std::exception const& error)
    {
      if (async::isOperationCancelled(error))
      {
        cancelledByException = true;
      }
      else
      {
        exceptionPtr = std::current_exception();
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      exceptionPtr = std::current_exception();
    }

    // Once a maintenance transaction may have committed, callback completion
    // is mandatory even if the caller requested cancellation in the meantime.
    try
    {
      co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();
    }
    // NOLINTNEXTLINE(aobus-async-cancellation-guard): This no-stop admission treats every live rejection as fatal.
    catch (...)
    {
      _implPtr->mutationService.handleFinalizationAdmissionFailure(std::current_exception());
      async::throwOperationCancelled();
    }

    maintenance.finish();
    backgroundTask.finish();

    if (cancelledByException)
    {
      async::throwOperationCancelled();
    }

    if (exceptionPtr)
    {
      std::rethrow_exception(exceptionPtr);
    }

    AO_INVARIANT(optRes);
    co_return std::move(*optRes);
  }

  async::Task<Result<>> LibraryTaskService::exportLibraryAsync(std::filesystem::path path,
                                                               rt::ExportMode mode,
                                                               std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    setCurrentThreadName("LibraryExport");
    auto result = Result<>{};
    auto exceptionPtr = std::exception_ptr{};
    bool cancelledByException = false;

    try
    {
      // Export only opens a read transaction; the LMDB snapshot is consistent
      // on its own, so it does not serialize against in-flight mutations.
      auto exporter = ao::rt::LibraryYamlExporter{_implPtr->library};
      result = exporter.exportToYaml(path, mode, stopToken);
    }
    catch (std::exception const& error)
    {
      if (async::isOperationCancelled(error))
      {
        cancelledByException = true;
      }
      else
      {
        exceptionPtr = std::current_exception();
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      exceptionPtr = std::current_exception();
    }

    // Resumed without the token so that a cancelled export still hands its
    // continuation back on the callback executor rather than throwing here, on a
    // worker.
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();

    if (cancelledByException)
    {
      async::throwOperationCancelled();
    }

    if (exceptionPtr)
    {
      std::rethrow_exception(exceptionPtr);
    }

    if (stopToken.stop_requested())
    {
      async::throwOperationCancelled();
    }

    co_return result;
  }

  async::Task<Result<ScanPlan>> LibraryTaskService::buildScanPlanAsync(std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
    auto optPlanRes = std::optional<Result<ScanPlan>>{};
    auto exceptionPtr = std::exception_ptr{};
    bool cancelledByException = false;

    try
    {
      co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
      setCurrentThreadName("LibraryScan");

      // Plan building only opens a read transaction; the LMDB snapshot is
      // consistent on its own, and holding the mutation mutex here would not
      // keep the plan fresh anyway (the lock is released before apply).
      auto scanService = LibraryScan{_implPtr->library};
      auto publishProgress = _implPtr->makeProgressPublisher();
      optPlanRes.emplace(scanService.buildPlan(
        [publishProgress = std::move(publishProgress)](std::filesystem::path const& path) mutable
        { publishProgress(LibraryTaskProgressKind::Scanning, 0.0, utility::pathToUtf8(path.filename())); },
        stopToken));
    }
    catch (std::exception const& error)
    {
      if (async::isOperationCancelled(error))
      {
        cancelledByException = true;
      }
      else
      {
        exceptionPtr = std::current_exception();
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      exceptionPtr = std::current_exception();
    }

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();
    _implPtr->notifyProgressFinished();

    if (cancelledByException)
    {
      async::throwOperationCancelled();
    }

    if (exceptionPtr)
    {
      std::rethrow_exception(exceptionPtr);
    }

    if (stopToken.stop_requested())
    {
      async::throwOperationCancelled();
    }

    AO_INVARIANT(optPlanRes);
    co_return std::move(*optPlanRes);
  }

  async::Task<Result<ScanApplyResult>> LibraryTaskService::applyScanPlanAsync(ScanPlan plan,
                                                                              ScanApplyOptions options,
                                                                              std::stop_token const stopToken,
                                                                              ScanProgressCallback progressCallback,
                                                                              ScanFailureCallback failureCallback)
  {
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
    auto backgroundTaskRes =
      _implPtr->mutationService.beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::ScanApply);

    if (!backgroundTaskRes)
    {
      _implPtr->notifyProgressFinished();
      co_return std::unexpected{backgroundTaskRes.error()};
    }

    auto backgroundTask = std::move(*backgroundTaskRes);
    auto coordinatedScanRes = Result<CoordinatedScanResult>{};
    auto const totalItems = plan.size();
    auto exceptionPtr = std::exception_ptr{};
    bool cancelledByException = false;

    try
    {
      co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
      setCurrentThreadName("ApplyScanPlan");
      auto progress =
        makeScanProgressReporter(totalItems, _implPtr->makeProgressPublisher(), std::move(progressCallback));
      auto failure = makeScanFailureReporter(std::move(failureCallback));

      coordinatedScanRes = applyCoordinatedScan(_implPtr->mutationService,
                                                backgroundTask,
                                                _implPtr->library,
                                                std::move(plan),
                                                options,
                                                std::move(progress),
                                                std::move(failure),
                                                stopToken);
    }
    catch (std::exception const& error)
    {
      if (async::isOperationCancelled(error))
      {
        cancelledByException = true;
      }
      else
      {
        exceptionPtr = std::current_exception();
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      exceptionPtr = std::current_exception();
    }

    try
    {
      co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();
    }
    // NOLINTNEXTLINE(aobus-async-cancellation-guard): This no-stop admission treats every live rejection as fatal.
    catch (...)
    {
      _implPtr->mutationService.handleFinalizationAdmissionFailure(std::current_exception());
      async::throwOperationCancelled();
    }

    backgroundTask.finish();
    _implPtr->notifyProgressFinished();

    if (cancelledByException)
    {
      async::throwOperationCancelled();
    }

    if (exceptionPtr)
    {
      std::rethrow_exception(exceptionPtr);
    }

    if (!coordinatedScanRes)
    {
      co_return std::unexpected{coordinatedScanRes.error()};
    }

    if (coordinatedScanRes->cancelled)
    {
      async::throwOperationCancelled();
    }

    if (coordinatedScanRes->result.staleCount > 0)
    {
      APP_LOG_INFO("Scan skipped {} stale item(s); a later scan will evaluate current evidence",
                   coordinatedScanRes->result.staleCount);
    }

    co_return Result<ScanApplyResult>{std::move(coordinatedScanRes->result)};
  }

  async::Task<Result<AudioIdentityIndexResult>> LibraryTaskService::backfillAudioIdentityAsync(
    std::stop_token const stopToken,
    AudioIdentityIndexProgressCallback progressCallback,
    AudioIdentityIndexFailureCallback failureCallback)
  {
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
    auto backgroundTaskRes =
      _implPtr->mutationService.beginBackgroundTask(LibraryMutationService::BackgroundTaskKind::AudioIdentityBackfill);

    if (!backgroundTaskRes)
    {
      _implPtr->notifyProgressFinished();
      co_return std::unexpected{backgroundTaskRes.error()};
    }

    auto backgroundTask = std::move(*backgroundTaskRes);
    auto backfillRes = Result<AudioIdentityIndexResult>{};
    auto exceptionPtr = std::exception_ptr{};
    bool cancelledByException = false;

    try
    {
      co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
      setCurrentThreadName("AudioBackfill");
      auto commitBatch = makeAudioIdentityCommitBatch(_implPtr->mutationService, backgroundTask);
      auto progress = makeAudioIdentityProgressReporter(_implPtr->makeProgressPublisher(), std::move(progressCallback));
      auto failure = makeAudioIdentityFailureReporter(std::move(failureCallback));

      // Fingerprinting runs without mutationService writer ownership; each
      // bounded write-back acquires its own background mutation.
      auto indexer = AudioIdentityIndexer{_implPtr->asyncRuntime, _implPtr->library};
      backfillRes =
        co_await indexer.indexPending(std::move(commitBatch), {}, std::move(progress), std::move(failure), stopToken);
    }
    catch (std::exception const& error)
    {
      if (async::isOperationCancelled(error))
      {
        cancelledByException = true;
      }
      else
      {
        exceptionPtr = std::current_exception();
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      exceptionPtr = std::current_exception();
    }

    try
    {
      co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();
    }
    // NOLINTNEXTLINE(aobus-async-cancellation-guard): This no-stop admission treats every live rejection as fatal.
    catch (...)
    {
      _implPtr->mutationService.handleFinalizationAdmissionFailure(std::current_exception());
      async::throwOperationCancelled();
    }

    backgroundTask.finish();
    _implPtr->notifyProgressFinished();

    if (cancelledByException)
    {
      async::throwOperationCancelled();
    }

    if (exceptionPtr)
    {
      std::rethrow_exception(exceptionPtr);
    }

    if (!backfillRes)
    {
      co_return std::unexpected{backfillRes.error()};
    }

    co_return backfillRes;
  }
} // namespace ao::rt
