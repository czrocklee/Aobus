// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/async/Executor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/AudioIdentityIndexer.h>
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
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
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
  } // namespace

  struct LibraryTaskService::Impl final
  {
    [[noreturn]] void dispatchFailureCompletionAndRethrow(std::exception_ptr const& exceptionPtr)
    {
      auto* const changesRaw = &changes;
      asyncRuntime.callbackExecutor().dispatch([changesRaw] { changesRaw->notifyLibraryTaskCompleted(0); });
      std::rethrow_exception(exceptionPtr);
    }

    async::Runtime& asyncRuntime;
    library::MusicLibrary& library;
    LibraryChanges& changes;
    std::mutex mutationMutex;
  };

  LibraryTaskService::LibraryTaskService(async::Runtime& asyncRuntime,
                                         library::MusicLibrary& library,
                                         LibraryChanges& changes)
    : _implPtr{std::make_unique<Impl>(asyncRuntime, library, changes)}
  {
  }

  LibraryTaskService::~LibraryTaskService() = default;

  async::Task<Result<>> LibraryTaskService::importLibraryAsync(std::filesystem::path path,
                                                               std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    setCurrentThreadName("LibraryImport");
    auto result = Result<>{};

    {
      auto mutationLock = std::scoped_lock{_implPtr->mutationMutex};
      auto importer = ao::rt::LibraryYamlImporter{_implPtr->library, _implPtr->changes};

      if (auto importResult = importer.importFromYaml(path); !importResult)
      {
        result = std::unexpected{importResult.error()};
      }
    }

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
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

    auto planResult = Result<ScanPlan>{};

    {
      // Plan building only opens a read transaction; the LMDB snapshot is
      // consistent on its own, and holding the mutation mutex here would not
      // keep the plan fresh anyway (the lock is released before apply).
      auto scanService = LibraryScan{_implPtr->library};
      planResult = scanService.buildPlan(
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
    }

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);

    if (!planResult)
    {
      // A scan that could not even begin (missing root, failed walk) is fatal to
      // the whole task. Clear any in-flight progress and report it as a failure.
      _implPtr->changes.notifyLibraryTaskCompleted(0);
      co_return std::unexpected{planResult.error()};
    }

    if (planResult->count(ScanClassification::New) == 0 && planResult->count(ScanClassification::Changed) == 0 &&
        planResult->count(ScanClassification::Moved) == 0 && planResult->count(ScanClassification::Missing) == 0)
    {
      _implPtr->changes.notifyLibraryTaskCompleted(0);
    }

    co_return std::move(planResult);
  }

  async::Task<Result<ScanApplyResult>> LibraryTaskService::applyScanPlanAsync(ScanPlan plan,
                                                                              ScanApplyOptions options,
                                                                              std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    setCurrentThreadName("ApplyScanPlan");

    auto applyResult = Result<ScanApplyResult>{};
    auto const totalItems = plan.items.size();
    auto exceptionPtr = std::exception_ptr{};

    {
      auto mutationLock = std::scoped_lock{_implPtr->mutationMutex};

      auto scanService = LibraryScan{_implPtr->library};

      try
      {
        applyResult = scanService.applyPlan(
          std::move(plan),
          options,
          [this, totalItems](ScanApplyProgress const& progress)
          {
            auto const itemBase = static_cast<double>(progress.itemIndex);
            auto const fraction =
              totalItems > 0 ? (itemBase + progress.itemFraction) / static_cast<double>(totalItems) : 0.0;
            auto message = scanApplyProgressMessage(progress);
            _implPtr->asyncRuntime.callbackExecutor().dispatch(
              [this, fraction, message = std::move(message)]
              {
                _implPtr->changes.notifyLibraryTaskProgress(LibraryChanges::LibraryTaskProgressUpdated{
                  .fraction = fraction,
                  .message = std::move(message),
                });
              });
          },
          // Diagnostics run on the worker thread; spdlog is thread-safe and the
          // failure's string views are only valid for the duration of this call.
          [](ScanFailure const& failure)
          {
            if (failure.uri.empty())
            {
              APP_LOG_ERROR("Failed to {}: {}", failure.stage, failure.message);
            }
            else
            {
              APP_LOG_ERROR("Failed to {} {}: {}", failure.stage, failure.uri, failure.message);
            }
          },
          stopToken);
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

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);

    if (!applyResult)
    {
      _implPtr->changes.notifyLibraryTaskCompleted(0);
      co_return std::unexpected{applyResult.error()};
    }

    auto const& result = *applyResult;
    auto const processedCount = result.insertedIds.size() + result.mutatedIds.size() + result.relinkedIds.size();
    _implPtr->changes.notifyLibraryTaskCompleted(processedCount);

    if (!result.cancelled && result.libraryRevision != 0)
    {
      auto mutatedIds = result.mutatedIds;
      mutatedIds.append_range(result.relinkedIds);
      _implPtr->changes.publish(LibraryChangeSet{.libraryRevision = result.libraryRevision,
                                                 .tracksInserted = result.insertedIds,
                                                 .tracksMutated = std::move(mutatedIds)});
    }

    co_return applyResult;
  }

  async::Task<Result<AudioIdentityIndexResult>> LibraryTaskService::backfillAudioIdentityAsync(
    std::stop_token const stopToken)
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
    setCurrentThreadName("AudioBackfill");

    auto backfillResult = Result<AudioIdentityIndexResult>{};
    auto exceptionPtr = std::exception_ptr{};

    {
      // No mutation lock here: fingerprinting runs unlocked and the indexer
      // acquires the lock itself, only around each batch write-back, so scans
      // and imports are not blocked while files are being hashed.
      auto indexer =
        AudioIdentityIndexer{_implPtr->asyncRuntime, _implPtr->library, _implPtr->mutationMutex, _implPtr->changes};

      try
      {
        backfillResult = co_await indexer.indexPending(
          {},
          [this](AudioIdentityIndexProgress const& progress)
          {
            if (!shouldPublishBackfillProgress(progress))
            {
              return;
            }

            auto const fraction = backfillProgressFraction(progress);
            auto message = "Indexing audio identity: " + progress.path.filename().string();
            _implPtr->asyncRuntime.callbackExecutor().dispatch(
              [this, fraction, message = std::move(message)]
              {
                _implPtr->changes.notifyLibraryTaskProgress(LibraryChanges::LibraryTaskProgressUpdated{
                  .fraction = fraction,
                  .message = std::move(message),
                });
              });
          },
          [](AudioIdentityIndexFailure const& failure)
          {
            if (failure.uri.empty())
            {
              APP_LOG_ERROR("Failed to {}: {}", failure.stage, failure.message);
            }
            else
            {
              APP_LOG_ERROR("Failed to {} {}: {}", failure.stage, failure.uri, failure.message);
            }
          },
          stopToken);
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

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);

    if (!backfillResult)
    {
      _implPtr->changes.notifyLibraryTaskCompleted(0);
      co_return std::unexpected{backfillResult.error()};
    }

    _implPtr->changes.notifyLibraryTaskCompleted(static_cast<std::size_t>(backfillResult->completedCount));
    co_return backfillResult;
  }
} // namespace ao::rt
