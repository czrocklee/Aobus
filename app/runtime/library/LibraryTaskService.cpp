// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AudioIdentityBatchWriter.h"
#include "LibraryMutationService.h"
#include "LibraryYamlImportOperation.h"
#include "ScanApplyOperation.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/library/MetadataLayout.h>
#include <ao/library/MetadataStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/AudioIdentityIndex.h>
#include <ao/rt/library/AudioIdentityIndexer.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryImportPlan.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/LibraryTaskEvents.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Path.h>
#include <ao/utility/ThreadName.h>

#include <gsl-lite/gsl-lite.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
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
    gsl_Expects(_implPtr != nullptr);
    return _implPtr->report;
  }

  namespace
  {
    using LibraryTaskProgressPublisher =
      std::move_only_function<void(LibraryTaskProgressKind kind, double fraction, std::string subject)>;

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

    Result<CoordinatedScanResult> applyCoordinatedScan(LibraryMutationService& mutationService,
                                                       LibraryMutationService::MaintenanceGuard const& maintenance,
                                                       library::MusicLibrary& library,
                                                       ScanPlan plan,
                                                       ScanApplyOptions options,
                                                       std::move_only_function<void(ScanApplyProgress const&)> progress,
                                                       std::move_only_function<void(ScanFailure const&)> failure,
                                                       std::stop_token stopToken)
    {
      auto operation = ScanApplyOperation{library, std::move(plan), std::move(progress), std::move(failure), options};
      auto prepareResult = operation.prepare(stopToken);

      if (!prepareResult)
      {
        return std::unexpected{prepareResult.error()};
      }

      if (operation.cancelled())
      {
        return CoordinatedScanResult{.result = std::move(*prepareResult), .cancelled = true};
      }

      auto revalidationResult = operation.revalidatePreparedFiles(stopToken);

      if (!revalidationResult)
      {
        return std::unexpected{revalidationResult.error()};
      }

      if (operation.cancelled())
      {
        return CoordinatedScanResult{.result = std::move(*revalidationResult), .cancelled = true};
      }

      if (!operation.readyForMutation())
      {
        return CoordinatedScanResult{.result = std::move(*revalidationResult)};
      }

      auto mutationResult = mutationService.beginMaintenanceMutation(maintenance);

      if (!mutationResult)
      {
        return std::unexpected{mutationResult.error()};
      }

      auto mutation = std::move(*mutationResult);
      auto applyResult = operation.apply(mutation.transaction(), stopToken);

      if (!applyResult)
      {
        return std::unexpected{applyResult.error()};
      }

      if (operation.cancelled())
      {
        return CoordinatedScanResult{.result = std::move(*applyResult), .cancelled = true};
      }

      if (!operation.transactionShouldCommit())
      {
        return CoordinatedScanResult{.result = std::move(*applyResult)};
      }

      auto mutatedIds = applyResult->mutatedIds;
      mutatedIds.append_range(applyResult->relinkedIds);
      auto commitResult = mutation.commit(
        LibraryChangeSet{.tracksInserted = applyResult->insertedIds, .tracksMutated = std::move(mutatedIds)});

      if (!commitResult)
      {
        return std::unexpected{commitResult.error()};
      }

      applyResult->libraryRevision = commitResult->libraryRevision;
      return CoordinatedScanResult{.result = std::move(*applyResult)};
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
      LibraryMutationService::MaintenanceGuard const& maintenance,
      library::MusicLibrary& library)
    {
      return [mutationServiceRaw = &mutationService, maintenanceRaw = &maintenance, libraryRaw = &library](
               std::span<AudioIdentityWriteCandidate const> candidates) -> Result<AudioIdentityBatchCommitResult>
      {
        auto mutationResult = mutationServiceRaw->beginMaintenanceMutation(*maintenanceRaw);

        if (!mutationResult)
        {
          return std::unexpected{mutationResult.error()};
        }

        auto mutation = std::move(*mutationResult);
        auto result = applyAudioIdentityBatch(*libraryRaw, mutation.transaction(), candidates);

        if (!result || result->completedCount == 0)
        {
          return result;
        }

        if (auto commitResult = mutation.commit(LibraryChangeSet{}); !commitResult)
        {
          return std::unexpected{commitResult.error()};
        }

        return result;
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

    LibraryTaskCompletionStatus completionStatus(AudioIdentityIndexResult const& result)
    {
      if (result.cancelled)
      {
        return LibraryTaskCompletionStatus::Cancelled;
      }

      if (result.failureCount == 0)
      {
        return LibraryTaskCompletionStatus::Succeeded;
      }

      return LibraryTaskCompletionStatus::CompletedWithIssues;
    }
  } // namespace

  struct LibraryTaskService::Impl final
  {
    struct Signals final
    {
      async::Signal<LibraryTaskCompleted const&> completed;
      async::Signal<LibraryTaskProgressUpdated const&> progress;
    };

    Impl(async::Runtime& runtimeRef, library::MusicLibrary& libraryRef, LibraryMutationService& mutationServiceRef)
      : asyncRuntime{runtimeRef}, library{libraryRef}, mutationService{mutationServiceRef}
    {
    }

    LibraryTaskProgressPublisher makeProgressPublisher()
    {
      auto* const executorRaw = &asyncRuntime.callbackExecutor();
      auto const weakSignalsPtr = std::weak_ptr<Signals>{signalsPtr};

      return [executorRaw, weakSignalsPtr](LibraryTaskProgressKind kind, double fraction, std::string subject)
      {
        executorRaw->dispatch(
          [weakSignalsPtr, kind, fraction, subject = std::move(subject)]
          {
            if (auto const signalsPtr = weakSignalsPtr.lock(); signalsPtr != nullptr)
            {
              auto const event = LibraryTaskProgressUpdated{
                .kind = kind,
                .fraction = fraction,
                .subject = std::move(subject),
              };
              signalsPtr->progress.emit(event);
            }
          });
      };
    }

    [[noreturn]] void dispatchFailureCompletionAndRethrow(std::exception_ptr const& exceptionPtr)
    {
      auto const weakSignalsPtr = std::weak_ptr<Signals>{signalsPtr};
      asyncRuntime.callbackExecutor().dispatch(
        [weakSignalsPtr]
        {
          if (auto const signalsPtr = weakSignalsPtr.lock(); signalsPtr != nullptr)
          {
            auto const event = LibraryTaskCompleted{.status = LibraryTaskCompletionStatus::Failed};
            signalsPtr->completed.emit(event);
          }
        });
      std::rethrow_exception(exceptionPtr);
    }

    void notifyCompleted(LibraryTaskCompletionStatus const status, std::size_t const affectedCount = 0)
    {
      auto const event = LibraryTaskCompleted{.status = status, .affectedCount = affectedCount};
      signalsPtr->completed.emit(event);
    }

    async::Runtime& asyncRuntime;
    library::MusicLibrary& library;
    LibraryMutationService& mutationService;
    std::shared_ptr<Signals> signalsPtr = std::make_shared<Signals>();
  };

  LibraryTaskService::LibraryTaskService(async::Runtime& asyncRuntime,
                                         library::MusicLibrary& library,
                                         LibraryMutationService& mutationService)
    : _implPtr{std::make_unique<Impl>(asyncRuntime, library, mutationService)}
  {
  }

  LibraryTaskService::~LibraryTaskService() = default;

  async::Subscription LibraryTaskService::onCompleted(
    std::move_only_function<void(LibraryTaskCompleted const&) noexcept> handler) const
  {
    return _implPtr->signalsPtr->completed.connect(std::move(handler));
  }

  async::Subscription LibraryTaskService::onProgress(
    std::move_only_function<void(LibraryTaskProgressUpdated const&) noexcept> handler) const
  {
    return _implPtr->signalsPtr->progress.connect(std::move(handler));
  }

  async::Task<Result<std::optional<std::vector<std::byte>>>> LibraryTaskService::loadResourceAsync(
    ResourceId const resourceId,
    std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);

    if (resourceId == kInvalidResourceId)
    {
      co_return std::optional<std::vector<std::byte>>{};
    }

    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    auto result = Result<std::optional<std::vector<std::byte>>>{};

    {
      auto transaction = _implPtr->library.readTransaction();
      auto const optBytes = _implPtr->library.resources().reader(transaction).get(resourceId);

      if (!optBytes)
      {
        result = std::optional<std::vector<std::byte>>{};
      }
      else if (optBytes->size() > kMaximumInteractiveResourceBytes)
      {
        result = makeError(Error::Code::ValueTooLarge, "Interactive resource exceeds the encoded-byte limit");
      }
      else
      {
        result = std::optional{std::vector<std::byte>{optBytes->begin(), optBytes->end()}};
      }
    }

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
    co_return result;
  }

  async::Task<Result<LibraryImportPlan>> LibraryTaskService::prepareLibraryImportAsync(std::filesystem::path path,
                                                                                       ImportMode const mode,
                                                                                       std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
    auto maintenanceResult = _implPtr->mutationService.beginMaintenance(LibraryMaintenanceKind::Import);

    if (!maintenanceResult)
    {
      co_return std::unexpected{maintenanceResult.error()};
    }

    auto maintenance = std::move(*maintenanceResult);
    auto const availability = _implPtr->mutationService.availability();
    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    setCurrentThreadName("LibraryImportPreview");

    auto result = [&] -> Result<LibraryImportPlan>
    {
      auto importer = ao::rt::LibraryYamlImporter{_implPtr->library};
      auto importOperation = LibraryYamlImportOperation{importer};
      auto preparedResult = importOperation.prepare(path, mode, true);

      if (!preparedResult)
      {
        return std::unexpected{preparedResult.error()};
      }

      auto targetLibraryId = std::array<std::byte, 16>{};
      std::uint64_t targetRevision = 0;

      {
        auto readTransaction = _implPtr->library.readTransaction();
        auto const headerResult = _implPtr->library.metadata().load(readTransaction);

        if (!headerResult)
        {
          return std::unexpected{headerResult.error()};
        }

        targetLibraryId = headerResult->libraryId;
        targetRevision = _implPtr->library.libraryRevision(readTransaction);
      }

      auto mutationResult = _implPtr->mutationService.beginMaintenanceMutation(maintenance);

      if (!mutationResult)
      {
        return std::unexpected{mutationResult.error()};
      }

      auto mutation = std::move(*mutationResult);
      auto reportResult = importOperation.preview(*preparedResult, mutation.transaction());

      if (!reportResult)
      {
        return std::unexpected{reportResult.error()};
      }

      return LibraryImportPlan{std::make_unique<LibraryImportPlan::Impl>(
        std::move(*preparedResult), *reportResult, targetLibraryId, availability.runtimeInstanceId, targetRevision)};
    }();

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
    co_return std::move(result);
  }

  async::Task<Result<ImportReport>> LibraryTaskService::applyLibraryImportPlanAsync(LibraryImportPlan plan,
                                                                                    std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);

    if (!plan._implPtr)
    {
      co_return makeError(Error::Code::InvalidState, "Import plan has already been consumed");
    }

    auto maintenanceResult = _implPtr->mutationService.beginMaintenance(LibraryMaintenanceKind::Import);

    if (!maintenanceResult)
    {
      co_return std::unexpected{maintenanceResult.error()};
    }

    auto maintenance = std::move(*maintenanceResult);
    auto const availability = _implPtr->mutationService.availability();
    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    setCurrentThreadName("LibraryImport");

    auto result = [&] -> Result<ImportReport>
    {
      auto const& binding = *plan._implPtr;

      if (availability.runtimeInstanceId != binding.runtimeInstanceId)
      {
        return makeError(Error::Code::Conflict, "Import plan belongs to a different library runtime");
      }

      {
        auto readTransaction = _implPtr->library.readTransaction();
        auto const headerResult = _implPtr->library.metadata().load(readTransaction);

        if (!headerResult)
        {
          return std::unexpected{headerResult.error()};
        }

        if (headerResult->libraryId != binding.targetLibraryId ||
            _implPtr->library.libraryRevision(readTransaction) != binding.targetRevision)
        {
          return makeError(Error::Code::Conflict, "Target library changed after the import preview");
        }
      }

      auto importer = ao::rt::LibraryYamlImporter{_implPtr->library};
      auto importOperation = LibraryYamlImportOperation{importer};

      if (auto sourceResult = importOperation.revalidateSource(binding.prepared); !sourceResult)
      {
        return std::unexpected{sourceResult.error()};
      }

      auto mutationResult = _implPtr->mutationService.beginMaintenanceMutation(maintenance);

      if (!mutationResult)
      {
        return std::unexpected{mutationResult.error()};
      }

      auto mutation = std::move(*mutationResult);
      auto importResult = importOperation.apply(binding.prepared, mutation.transaction());

      if (!importResult)
      {
        return std::unexpected{importResult.error()};
      }

      auto changeSet = importOperation.buildChangeSet(binding.prepared, mutation.transaction());

      if (auto commitResult = mutation.commit(std::move(changeSet)); !commitResult)
      {
        return std::unexpected{commitResult.error()};
      }

      return *importResult;
    }();

    // Once a maintenance transaction may have committed, callback completion
    // is mandatory even if the caller requested cancellation in the meantime.
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();
    co_return result;
  }

  async::Task<Result<>> LibraryTaskService::exportLibraryAsync(std::filesystem::path path,
                                                               rt::ExportMode mode,
                                                               std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    setCurrentThreadName("LibraryExport");
    auto result = Result<>{};

    {
      // Export only opens a read transaction; the LMDB snapshot is consistent
      // on its own, so it does not serialize against in-flight mutations.
      auto exporter = ao::rt::LibraryYamlExporter{_implPtr->library};
      result = exporter.exportToYaml(path, mode);
    }

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
    co_return result;
  }

  async::Task<Result<ScanPlan>> LibraryTaskService::buildScanPlanAsync(std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    setCurrentThreadName("LibraryScan");

    // Plan building only opens a read transaction; the LMDB snapshot is
    // consistent on its own, and holding the mutation mutex here would not
    // keep the plan fresh anyway (the lock is released before apply).
    auto scanService = LibraryScan{_implPtr->library};
    auto publishProgress = _implPtr->makeProgressPublisher();
    auto planResult = scanService.buildPlan(
      [publishProgress = std::move(publishProgress)](std::filesystem::path const& path) mutable
      { publishProgress(LibraryTaskProgressKind::Scanning, 0.0, utility::pathToUtf8(path.filename())); });

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();

    if (stopToken.stop_requested())
    {
      _implPtr->notifyCompleted(LibraryTaskCompletionStatus::Cancelled);
      async::throwOperationCancelled();
    }

    if (!planResult)
    {
      // A scan that could not even begin (missing root, failed walk) is fatal to
      // the whole task. Clear any in-flight progress and report it as a failure.
      _implPtr->notifyCompleted(LibraryTaskCompletionStatus::Failed);
      co_return std::unexpected{planResult.error()};
    }

    if (planResult->count(ScanClassification::New) == 0 && planResult->count(ScanClassification::Changed) == 0 &&
        planResult->count(ScanClassification::Moved) == 0 && planResult->count(ScanClassification::Missing) == 0)
    {
      _implPtr->notifyCompleted(planResult->count(ScanClassification::Error) == 0
                                  ? LibraryTaskCompletionStatus::Succeeded
                                  : LibraryTaskCompletionStatus::CompletedWithIssues);
    }

    co_return std::move(planResult);
  }

  async::Task<Result<ScanApplyResult>> LibraryTaskService::applyScanPlanAsync(ScanPlan plan,
                                                                              ScanApplyOptions options,
                                                                              std::stop_token const stopToken,
                                                                              ScanProgressCallback progressCallback,
                                                                              ScanFailureCallback failureCallback)
  {
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
    auto maintenanceResult = _implPtr->mutationService.beginMaintenance(LibraryMaintenanceKind::ScanApply);

    if (!maintenanceResult)
    {
      co_return std::unexpected{maintenanceResult.error()};
    }

    auto maintenance = std::move(*maintenanceResult);
    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    setCurrentThreadName("ApplyScanPlan");

    auto coordinatedScan = Result<CoordinatedScanResult>{};
    auto const totalItems = plan.size();
    auto exceptionPtr = std::exception_ptr{};

    try
    {
      auto progress =
        makeScanProgressReporter(totalItems, _implPtr->makeProgressPublisher(), std::move(progressCallback));
      auto failure = makeScanFailureReporter(std::move(failureCallback));

      coordinatedScan = applyCoordinatedScan(_implPtr->mutationService,
                                             maintenance,
                                             _implPtr->library,
                                             std::move(plan),
                                             options,
                                             std::move(progress),
                                             std::move(failure),
                                             stopToken);
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      exceptionPtr = std::current_exception();
    }

    if (exceptionPtr)
    {
      _implPtr->dispatchFailureCompletionAndRethrow(exceptionPtr);
    }

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();

    if (!coordinatedScan)
    {
      _implPtr->notifyCompleted(LibraryTaskCompletionStatus::Failed);
      co_return std::unexpected{coordinatedScan.error()};
    }

    auto const& result = coordinatedScan->result;
    auto const processedCount = result.insertedIds.size() + result.mutatedIds.size() + result.relinkedIds.size();

    if (coordinatedScan->cancelled)
    {
      _implPtr->notifyCompleted(LibraryTaskCompletionStatus::Cancelled);
      async::throwOperationCancelled();
    }

    _implPtr->notifyCompleted(result.failureCount == 0 ? LibraryTaskCompletionStatus::Succeeded
                                                       : LibraryTaskCompletionStatus::CompletedWithIssues,
                              processedCount);

    co_return Result<ScanApplyResult>{std::move(coordinatedScan->result)};
  }

  async::Task<Result<AudioIdentityIndexResult>> LibraryTaskService::backfillAudioIdentityAsync(
    std::stop_token const stopToken,
    AudioIdentityIndexProgressCallback progressCallback,
    AudioIdentityIndexFailureCallback failureCallback)
  {
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
    auto maintenanceResult = _implPtr->mutationService.beginMaintenance(LibraryMaintenanceKind::AudioIdentityBackfill);

    if (!maintenanceResult)
    {
      co_return std::unexpected{maintenanceResult.error()};
    }

    auto maintenance = std::move(*maintenanceResult);
    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    setCurrentThreadName("AudioBackfill");

    auto backfillResult = Result<AudioIdentityIndexResult>{};
    auto exceptionPtr = std::exception_ptr{};

    {
      auto commitBatch = makeAudioIdentityCommitBatch(_implPtr->mutationService, maintenance, _implPtr->library);
      auto progress = makeAudioIdentityProgressReporter(_implPtr->makeProgressPublisher(), std::move(progressCallback));
      auto failure = makeAudioIdentityFailureReporter(std::move(failureCallback));

      // Fingerprinting runs without mutationService writer ownership; each
      // bounded write-back acquires its own maintenance mutation.
      auto indexer = AudioIdentityIndexer{_implPtr->asyncRuntime, _implPtr->library};

      try
      {
        backfillResult =
          co_await indexer.indexPending(std::move(commitBatch), {}, std::move(progress), std::move(failure), stopToken);
      }
      catch (...)
      {
        async::rethrowIfOperationCancelled();
        exceptionPtr = std::current_exception();
      }
    }

    if (exceptionPtr)
    {
      _implPtr->dispatchFailureCompletionAndRethrow(exceptionPtr);
    }

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();

    if (!backfillResult)
    {
      _implPtr->notifyCompleted(LibraryTaskCompletionStatus::Failed);
      co_return std::unexpected{backfillResult.error()};
    }

    _implPtr->notifyCompleted(
      completionStatus(*backfillResult), static_cast<std::size_t>(backfillResult->completedCount));
    co_return backfillResult;
  }
} // namespace ao::rt
