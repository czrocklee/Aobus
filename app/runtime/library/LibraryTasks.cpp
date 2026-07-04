// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryScanner.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ScanPlanExecutor.h>
#include <ao/rt/Log.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryTasks.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/utility/ThreadUtils.h>

#include <boost/asio/this_coro.hpp>

#include <exception>
#include <expected>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    std::string scanApplyProgressMessage(library::ScanPlanExecutor::Progress const& progress)
    {
      auto prefix = std::string_view{"Updating"};

      if (progress.stage == library::ScanPlanExecutor::ProgressStage::Fingerprinting)
      {
        prefix = "Fingerprinting";
      }

      return std::string{prefix} + ": " + progress.path.filename().string();
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

  struct LibraryTasks::Impl final
  {
    async::Runtime& asyncRuntime;
    library::MusicLibrary& library;
    LibraryChanges& changes;
  };

  LibraryTasks::LibraryTasks(async::Runtime& asyncRuntime, library::MusicLibrary& library, LibraryChanges& changes)
    : _implPtr{std::make_unique<Impl>(asyncRuntime, library, changes)}
  {
  }

  LibraryTasks::~LibraryTasks() = default;

  async::Task<Result<>> LibraryTasks::importLibraryAsync(std::filesystem::path path)
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker();
    setCurrentThreadName("LibraryImport");
    auto importer = ao::rt::LibraryYamlImporter{_implPtr->library};
    auto importResult = importer.importFromYaml(path);
    auto result = Result<>{};

    if (!importResult)
    {
      result = std::unexpected{importResult.error()};
    }

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();
    co_return result;
  }

  async::Task<Result<>> LibraryTasks::exportLibraryAsync(std::filesystem::path path, rt::ExportMode mode)
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker();
    setCurrentThreadName("LibraryExport");
    auto exporter = ao::rt::LibraryYamlExporter{_implPtr->library};
    auto result = exporter.exportToYaml(path, mode);

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();
    co_return result;
  }

  async::Task<Result<library::ScanPlan>> LibraryTasks::buildScanPlanAsync()
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker();
    setCurrentThreadName("LibraryScanner");

    auto scanner = library::LibraryScanner{_implPtr->library};
    auto planResult = scanner.buildPlan(
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

    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor();

    if (!planResult)
    {
      // A scan that could not even begin (missing root, failed walk) is fatal to
      // the whole task. Clear any in-flight progress and report it as a failure.
      _implPtr->changes.notifyLibraryTaskCompleted(0);
      co_return std::unexpected{planResult.error()};
    }

    if (planResult->count(library::ScanClassification::New) == 0 &&
        planResult->count(library::ScanClassification::Changed) == 0 &&
        planResult->count(library::ScanClassification::Moved) == 0 &&
        planResult->count(library::ScanClassification::Missing) == 0)
    {
      _implPtr->changes.notifyLibraryTaskCompleted(0);
    }

    co_return std::move(planResult);
  }

  async::Task<Result<library::ScanApplyResult>> LibraryTasks::applyScanPlanAsync(library::ScanPlan plan)
  {
    co_await _implPtr->asyncRuntime.resumeOnWorker();
    setCurrentThreadName("ApplyScanPlan");

    auto applyResult = Result<library::ScanApplyResult>{};
    auto const totalItems = plan.items.size();

    auto executor = ao::library::ScanPlanExecutor{
      _implPtr->library,
      std::move(plan),
      [this, totalItems](library::ScanPlanExecutor::Progress const& progress)
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
      [](library::ScanFailure const& failure)
      {
        if (failure.uri.empty())
        {
          APP_LOG_ERROR("Failed to {}: {}", failure.stage, failure.message);
        }
        else
        {
          APP_LOG_ERROR("Failed to {} {}: {}", failure.stage, failure.uri, failure.message);
        }
      }};

    auto exceptionPtr = std::exception_ptr{};

    try
    {
      auto stopSource = std::stop_source{};
      auto const cancellationState = co_await boost::asio::this_coro::cancellation_state;
      auto slotGuard = CancellationSlotGuard{cancellationState.slot()};

      if (slotGuard.slot.is_connected())
      {
        slotGuard.slot.assign([&stopSource](async::CancellationType) { stopSource.request_stop(); });
      }

      applyResult = executor.run(stopSource.get_token());
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      exceptionPtr = std::current_exception();
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
} // namespace ao::rt
