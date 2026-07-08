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

#include <boost/asio/this_coro.hpp>

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

    struct [[nodiscard]] CancellationSlotGuard final
    {
      async::CancellationSlot slot;

      explicit CancellationSlotGuard(async::CancellationSlot slotValue)
        : slot{slotValue}
      {
      }

      ~CancellationSlotGuard()
      {
        if (slot.has_handler())
        {
          slot.clear();
        }
      }

      CancellationSlotGuard(CancellationSlotGuard const&) = delete;
      CancellationSlotGuard& operator=(CancellationSlotGuard const&) = delete;
      CancellationSlotGuard(CancellationSlotGuard&&) = delete;
      CancellationSlotGuard& operator=(CancellationSlotGuard&&) = delete;
    };
  } // namespace

  struct LibraryTaskService::Impl final
  {
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

  async::Task<Result<>> LibraryTaskService::importLibraryAsync(std::filesystem::path path)
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker();
    setCurrentThreadName("LibraryImport");
    auto result = Result<>{};

    {
      auto mutationLock = std::scoped_lock{_implPtr->mutationMutex};
      auto importer = ao::rt::LibraryYamlImporter{_implPtr->library};

      if (auto importResult = importer.importFromYaml(path); !importResult)
      {
        result = std::unexpected{importResult.error()};
      }
    }

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();
    co_return result;
  }

  async::Task<Result<>> LibraryTaskService::exportLibraryAsync(std::filesystem::path path, rt::ExportMode mode)
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker();
    setCurrentThreadName("LibraryExport");
    auto result = Result<>{};

    {
      // Export only opens a read transaction; the LMDB snapshot is consistent
      // on its own, so it does not serialize against in-flight mutations.
      auto exporter = ao::rt::LibraryYamlExporter{_implPtr->library};
      result = exporter.exportToYaml(path, mode);
    }

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();
    co_return result;
  }

  async::Task<Result<ScanPlan>> LibraryTaskService::buildScanPlanAsync()
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker();
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
          _implPtr->asyncRuntime.callbackExecutor().dispatch(
            [this, path]
            {
              _implPtr->changes.notifyLibraryTaskProgress(LibraryChanges::LibraryTaskProgressUpdated{
                .fraction = 0.0,
                .message = "Scanning: " + path.filename().string(),
              });
            });
        });
    }

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();

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

  async::Task<Result<ScanApplyResult>> LibraryTaskService::applyScanPlanAsync(ScanPlan plan, ScanApplyOptions options)
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker();
    setCurrentThreadName("ApplyScanPlan");

    auto applyResult = Result<ScanApplyResult>{};
    auto const totalItems = plan.items.size();
    auto exceptionPtr = std::exception_ptr{};

    {
      auto stopSource = std::stop_source{};
      auto const cancellationState = co_await boost::asio::this_coro::cancellation_state;
      auto slotGuard = CancellationSlotGuard{cancellationState.slot()};

      if (slotGuard.slot.is_connected())
      {
        slotGuard.slot.assign([&stopSource](async::CancellationType) { stopSource.request_stop(); });
      }

      auto mutationLock = std::scoped_lock{_implPtr->mutationMutex};

      auto scanService = LibraryScan{_implPtr->library};

      try
      {
        applyResult = scanService.applyPlan(
          std::move(plan),
          options,
          [this, totalItems](ScanApplyProgress const& progress)
          {
            _implPtr->asyncRuntime.callbackExecutor().dispatch(
              [this, progress, totalItems]
              {
                auto const itemBase = static_cast<double>(progress.itemIndex);
                auto const fraction =
                  totalItems > 0 ? (itemBase + progress.itemFraction) / static_cast<double>(totalItems) : 0.0;
                _implPtr->changes.notifyLibraryTaskProgress(LibraryChanges::LibraryTaskProgressUpdated{
                  .fraction = fraction,
                  .message = scanApplyProgressMessage(progress),
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
          stopSource.get_token());
      }
      catch (...)
      {
        async::rethrowIfOperationCancelled();
        exceptionPtr = std::current_exception();
      }
    }

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();

    if (exceptionPtr)
    {
      _implPtr->changes.notifyLibraryTaskCompleted(0);
      std::rethrow_exception(exceptionPtr);
    }

    if (!applyResult)
    {
      _implPtr->changes.notifyLibraryTaskCompleted(0);
      co_return std::unexpected{applyResult.error()};
    }

    auto const& result = *applyResult;
    _implPtr->changes.notifyLibraryTaskCompleted(result.processedIds.size());

    if (!result.cancelled && !result.processedIds.empty())
    {
      _implPtr->changes.notifyTracksMutated(result.processedIds);
    }

    co_return applyResult;
  }

  async::Task<Result<AudioIdentityIndexResult>> LibraryTaskService::backfillAudioIdentityAsync()
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker();
    setCurrentThreadName("AudioBackfill");

    auto backfillResult = Result<AudioIdentityIndexResult>{};
    auto exceptionPtr = std::exception_ptr{};

    {
      auto stopSource = std::stop_source{};
      auto const cancellationState = co_await boost::asio::this_coro::cancellation_state;
      auto slotGuard = CancellationSlotGuard{cancellationState.slot()};

      if (slotGuard.slot.is_connected())
      {
        slotGuard.slot.assign([&stopSource](async::CancellationType) { stopSource.request_stop(); });
      }

      // No mutation lock here: fingerprinting runs unlocked and the indexer
      // acquires the lock itself, only around each batch write-back, so scans
      // and imports are not blocked while files are being hashed.
      auto indexer = AudioIdentityIndexer{_implPtr->asyncRuntime, _implPtr->library, _implPtr->mutationMutex};

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
            auto const filename = progress.path.filename().string();
            _implPtr->asyncRuntime.callbackExecutor().dispatch(
              [this, fraction, filename]
              {
                _implPtr->changes.notifyLibraryTaskProgress(LibraryChanges::LibraryTaskProgressUpdated{
                  .fraction = fraction,
                  .message = "Indexing audio identity: " + filename,
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
          stopSource.get_token());
      }
      catch (...)
      {
        async::rethrowIfOperationCancelled();
        exceptionPtr = std::current_exception();
      }
    }

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();

    if (exceptionPtr)
    {
      _implPtr->changes.notifyLibraryTaskCompleted(0);
      std::rethrow_exception(exceptionPtr);
    }

    if (!backfillResult)
    {
      _implPtr->changes.notifyLibraryTaskCompleted(0);
      co_return std::unexpected{backfillResult.error()};
    }

    _implPtr->changes.notifyLibraryTaskCompleted(static_cast<std::size_t>(backfillResult->completedCount));
    co_return backfillResult;
  }
} // namespace ao::rt
