// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AudioIdentityBatchWriter.h"
#include "LibraryMutationService.h"
#include "LibraryYamlImportOperation.h"
#include "ScanApplyOperation.h"
#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/AudioIdentityIndexer.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryScan.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/ThreadName.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    using LibraryTaskCompletionStatus = LibraryChanges::LibraryTaskCompletionStatus;
    using LibraryTaskProgressPublisher = std::move_only_function<void(double fraction, std::string message)>;

    std::string scanApplyProgressMessage(ScanApplyProgress const& progress)
    {
      auto prefix = std::string_view{"Updating"};

      if (progress.stage == ScanApplyProgressStage::Fingerprinting)
      {
        prefix = "Fingerprinting";
      }

      return std::string{prefix} + ": " + progress.path.filename().string();
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

    struct CoordinatedScanOutcome final
    {
      ScanApplyResult result;
      bool cancelled = false;
    };

    Result<CoordinatedScanOutcome> applyCoordinatedScan(
      LibraryMutationService& mutationService,
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
        return CoordinatedScanOutcome{.result = std::move(*prepareResult), .cancelled = true};
      }

      auto revalidationResult = operation.revalidatePreparedFiles(stopToken);

      if (!revalidationResult)
      {
        return std::unexpected{revalidationResult.error()};
      }

      if (operation.cancelled())
      {
        return CoordinatedScanOutcome{.result = std::move(*revalidationResult), .cancelled = true};
      }

      if (!operation.readyForMutation())
      {
        return CoordinatedScanOutcome{.result = std::move(*revalidationResult)};
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
        return CoordinatedScanOutcome{.result = std::move(*applyResult), .cancelled = true};
      }

      if (!operation.transactionShouldCommit())
      {
        return CoordinatedScanOutcome{.result = std::move(*applyResult)};
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
      return CoordinatedScanOutcome{.result = std::move(*applyResult)};
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
        auto message = scanApplyProgressMessage(progress);
        publish(fraction, std::move(message));

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

    AudioIdentityIndexer::ProgressCallback makeAudioIdentityProgressReporter(
      LibraryTaskProgressPublisher publish,
      AudioIdentityIndexer::ProgressCallback callback)
    {
      return [publish = std::move(publish),
              callback = std::move(callback)](AudioIdentityIndexProgress const& progress) mutable
      {
        if (shouldPublishBackfillProgress(progress))
        {
          auto const fraction = backfillProgressFraction(progress);
          auto message = "Indexing audio identity: " + progress.path.filename().string();
          publish(fraction, std::move(message));
        }

        if (callback)
        {
          callback(progress);
        }
      };
    }

    AudioIdentityIndexer::ItemFailureCallback makeAudioIdentityFailureReporter(
      AudioIdentityIndexer::ItemFailureCallback callback)
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
    LibraryTaskProgressPublisher makeProgressPublisher()
    {
      return [this](double fraction, std::string message)
      {
        auto* const changesRaw = &changes;
        asyncRuntime.callbackExecutor().dispatch(
          [changesRaw, fraction, message = std::move(message)]
          {
            changesRaw->notifyLibraryTaskProgress(LibraryChanges::LibraryTaskProgressUpdated{
              .fraction = fraction,
              .message = std::move(message),
            });
          });
      };
    }

    [[noreturn]] void dispatchFailureCompletionAndRethrow(std::exception_ptr const& exceptionPtr)
    {
      auto* const changesRaw = &changes;
      asyncRuntime.callbackExecutor().dispatch(
        [changesRaw] { changesRaw->notifyLibraryTaskCompleted(LibraryTaskCompletionStatus::Failed); });
      std::rethrow_exception(exceptionPtr);
    }

    async::Runtime& asyncRuntime;
    library::MusicLibrary& library;
    LibraryChanges& changes;
    LibraryMutationService& mutationService;
  };

  LibraryTaskService::LibraryTaskService(async::Runtime& asyncRuntime,
                                         library::MusicLibrary& library,
                                         LibraryChanges& changes,
                                         LibraryMutationService& mutationService)
    : _implPtr{std::make_unique<Impl>(asyncRuntime, library, changes, mutationService)}
  {
  }

  LibraryTaskService::~LibraryTaskService() = default;

  async::Task<Result<ImportReport>> LibraryTaskService::importLibraryAsync(std::filesystem::path path,
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
    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    setCurrentThreadName("LibraryImport");
    auto result = Result<ImportReport>{ImportReport{}};
    auto importer = ao::rt::LibraryYamlImporter{_implPtr->library};
    auto importOperation = LibraryYamlImportOperation{importer};
    auto preparedResult = importOperation.prepare(path, mode, true);

    if (!preparedResult)
    {
      result = std::unexpected{preparedResult.error()};
    }
    else
    {
      auto mutationResult = _implPtr->mutationService.beginMaintenanceMutation(maintenance);

      if (!mutationResult)
      {
        result = std::unexpected{mutationResult.error()};
      }
      else
      {
        auto mutation = std::move(*mutationResult);
        auto changeSet = LibraryChangeSet{};
        auto importResult = importOperation.apply(*preparedResult, mutation.transaction(), changeSet);

        if (!importResult)
        {
          result = std::unexpected{importResult.error()};
        }
        else if (auto commitResult = mutation.commit(std::move(changeSet)); !commitResult)
        {
          result = std::unexpected{commitResult.error()};
        }
        else
        {
          result = *importResult;
        }
      }
    }

    // Once a maintenance transaction may have committed, callback completion
    // is mandatory even if the caller requested cancellation in the meantime.
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();
    co_return result;
  }

  async::Task<Result<ImportReport>> LibraryTaskService::previewLibraryImportAsync(std::filesystem::path path,
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
    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    setCurrentThreadName("LibraryImportPreview");
    auto result = Result<ImportReport>{ImportReport{}};
    auto importer = ao::rt::LibraryYamlImporter{_implPtr->library};
    auto importOperation = LibraryYamlImportOperation{importer};
    auto preparedResult = importOperation.prepare(path, mode, false);

    if (!preparedResult)
    {
      result = std::unexpected{preparedResult.error()};
    }
    else
    {
      auto mutationResult = _implPtr->mutationService.beginMaintenanceMutation(maintenance);

      if (!mutationResult)
      {
        result = std::unexpected{mutationResult.error()};
      }
      else
      {
        auto mutation = std::move(*mutationResult);
        result = importOperation.preview(*preparedResult, mutation.transaction());
      }
    }

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
    auto planResult = scanService.buildPlan(
      [this](std::filesystem::path const& path)
      {
        auto message = "Scanning: " + path.filename().string();
        _implPtr->asyncRuntime.callbackExecutor().dispatch(
          [this, message = std::move(message)]
          {
            _implPtr->changes.notifyLibraryTaskProgress(LibraryChanges::LibraryTaskProgressUpdated{
              .fraction = 0.0,
              .message = std::move(message),
            });
          });
      });

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();

    if (stopToken.stop_requested())
    {
      _implPtr->changes.notifyLibraryTaskCompleted(LibraryTaskCompletionStatus::Cancelled);
      async::throwOperationCancelled();
    }

    if (!planResult)
    {
      // A scan that could not even begin (missing root, failed walk) is fatal to
      // the whole task. Clear any in-flight progress and report it as a failure.
      _implPtr->changes.notifyLibraryTaskCompleted(LibraryTaskCompletionStatus::Failed);
      co_return std::unexpected{planResult.error()};
    }

    if (planResult->count(ScanClassification::New) == 0 && planResult->count(ScanClassification::Changed) == 0 &&
        planResult->count(ScanClassification::Moved) == 0 && planResult->count(ScanClassification::Missing) == 0)
    {
      _implPtr->changes.notifyLibraryTaskCompleted(planResult->count(ScanClassification::Error) == 0
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

    auto applyOutcome = Result<CoordinatedScanOutcome>{};
    auto const totalItems = plan.size();
    auto exceptionPtr = std::exception_ptr{};

    try
    {
      auto progress =
        makeScanProgressReporter(totalItems, _implPtr->makeProgressPublisher(), std::move(progressCallback));
      auto failure = makeScanFailureReporter(std::move(failureCallback));

      applyOutcome = applyCoordinatedScan(_implPtr->mutationService,
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

    if (!applyOutcome)
    {
      _implPtr->changes.notifyLibraryTaskCompleted(LibraryTaskCompletionStatus::Failed);
      co_return std::unexpected{applyOutcome.error()};
    }

    auto const& result = applyOutcome->result;
    auto const processedCount = result.insertedIds.size() + result.mutatedIds.size() + result.relinkedIds.size();

    if (applyOutcome->cancelled)
    {
      _implPtr->changes.notifyLibraryTaskCompleted(LibraryTaskCompletionStatus::Cancelled);
      async::throwOperationCancelled();
    }

    _implPtr->changes.notifyLibraryTaskCompleted(result.failureCount == 0
                                                   ? LibraryTaskCompletionStatus::Succeeded
                                                   : LibraryTaskCompletionStatus::CompletedWithIssues,
                                                 processedCount);

    co_return Result<ScanApplyResult>{std::move(applyOutcome->result)};
  }

  async::Task<Result<AudioIdentityIndexResult>> LibraryTaskService::backfillAudioIdentityAsync(
    std::stop_token const stopToken,
    AudioIdentityIndexer::ProgressCallback progressCallback,
    AudioIdentityIndexer::ItemFailureCallback failureCallback)
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
      _implPtr->changes.notifyLibraryTaskCompleted(LibraryTaskCompletionStatus::Failed);
      co_return std::unexpected{backfillResult.error()};
    }

    _implPtr->changes.notifyLibraryTaskCompleted(
      completionStatus(*backfillResult), static_cast<std::size_t>(backfillResult->completedCount));
    co_return backfillResult;
  }
} // namespace ao::rt
