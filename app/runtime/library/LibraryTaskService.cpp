// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/rt/library/LibraryTaskService.h>

#include "AudioIdentityBatchWriter.h"
#include "LibraryMutationService.h"
#include "LibraryYamlImportOperation.h"
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
#include <ao/library/LibraryWrite.h>
#include <ao/library/MetadataLayout.h>
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
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/rt/library/ScanPlan.h>
#include <ao/utility/Path.h>
#include <ao/utility/ThreadName.h>

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
    AO_EXPECTS(_implPtr != nullptr);
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

      auto mutationRes = mutationService.beginMaintenanceMutation(maintenance);

      if (!mutationRes)
      {
        return std::unexpected{mutationRes.error()};
      }

      auto mutation = std::move(*mutationRes);
      auto applyRes = mutation.apply([&operation, stopToken](library::LibraryWrite& transaction)
                                     { return operation.apply(transaction, stopToken); });

      if (!applyRes)
      {
        return std::unexpected{applyRes.error()};
      }

      if (operation.cancelled())
      {
        return CoordinatedScanResult{.result = std::move(*applyRes), .cancelled = true};
      }

      if (!operation.transactionShouldCommit())
      {
        return CoordinatedScanResult{.result = std::move(*applyRes)};
      }

      auto mutatedIds = applyRes->mutatedIds;
      mutatedIds.append_range(applyRes->relinkedIds);
      auto commitRes = mutation.commit(
        LibraryChangeSet{.tracksInserted = applyRes->insertedIds, .tracksMutated = std::move(mutatedIds)});

      if (!commitRes)
      {
        return std::unexpected{commitRes.error()};
      }

      applyRes->libraryRevision = commitRes->libraryRevision;
      return CoordinatedScanResult{.result = std::move(*applyRes)};
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
      LibraryMutationService::MaintenanceGuard const& maintenance)
    {
      return [mutationServiceRaw = &mutationService, maintenanceRaw = &maintenance](
               std::span<AudioIdentityWriteCandidate const> candidates) -> Result<AudioIdentityBatchCommitResult>
      {
        auto mutationRes = mutationServiceRaw->beginMaintenanceMutation(*maintenanceRaw);

        if (!mutationRes)
        {
          return std::unexpected{mutationRes.error()};
        }

        auto mutation = std::move(*mutationRes);
        auto result = mutation.apply([candidates](library::LibraryWrite& transaction)
                                     { return applyAudioIdentityBatch(transaction, candidates); });

        if (!result || result->completedCount == 0)
        {
          return result;
        }

        if (auto commitRes = mutation.commit(LibraryChangeSet{}); !commitRes)
        {
          return std::unexpected{commitRes.error()};
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
  } // namespace

  struct LibraryTaskService::Impl final
  {
    struct Signals final
    {
      async::Signal<> progressFinished;
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

    void notifyProgressFinished() const noexcept { signalsPtr->progressFinished.emit(); }

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

  async::Subscription LibraryTaskService::onProgressFinished(std::move_only_function<void()> handler) const
  {
    return _implPtr->signalsPtr->progressFinished.connect(std::move(handler));
  }

  async::Subscription LibraryTaskService::onProgress(
    std::move_only_function<void(LibraryTaskProgressUpdated const&)> handler) const
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

          return LibraryImportPlan{std::make_unique<LibraryImportPlan::Impl>(
            std::move(*preparedRes), *reportRes, targetLibraryId, availability.runtimeInstanceId, targetRevision)};
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
          auto importRes = mutation.apply([&importOperation, &binding](library::LibraryWrite& transaction)
                                          { return importOperation.apply(binding.prepared, transaction); });

          if (!importRes)
          {
            return std::unexpected{importRes.error()};
          }

          auto changeSetRes =
            mutation.apply([&importOperation, &binding](library::LibraryWrite& transaction) -> Result<LibraryChangeSet>
                           { return importOperation.buildChangeSet(binding.prepared, transaction); });

          if (!changeSetRes)
          {
            return std::unexpected{changeSetRes.error()};
          }

          if (auto commitRes = mutation.commit(std::move(*changeSetRes)); !commitRes)
          {
            return std::unexpected{commitRes.error()};
          }

          return *importRes;
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
        { publishProgress(LibraryTaskProgressKind::Scanning, 0.0, utility::pathToUtf8(path.filename())); }));
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
    auto maintenanceRes = _implPtr->mutationService.beginMaintenance(LibraryMaintenanceKind::ScanApply);

    if (!maintenanceRes)
    {
      _implPtr->notifyProgressFinished();
      co_return std::unexpected{maintenanceRes.error()};
    }

    auto maintenance = std::move(*maintenanceRes);
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
                                                maintenance,
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

    maintenance.finish();
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

    co_return Result<ScanApplyResult>{std::move(coordinatedScanRes->result)};
  }

  async::Task<Result<AudioIdentityIndexResult>> LibraryTaskService::backfillAudioIdentityAsync(
    std::stop_token const stopToken,
    AudioIdentityIndexProgressCallback progressCallback,
    AudioIdentityIndexFailureCallback failureCallback)
  {
    co_await _implPtr->asyncRuntime.resumeOnCallbackExecutor(stopToken);
    auto maintenanceRes = _implPtr->mutationService.beginMaintenance(LibraryMaintenanceKind::AudioIdentityBackfill);

    if (!maintenanceRes)
    {
      _implPtr->notifyProgressFinished();
      co_return std::unexpected{maintenanceRes.error()};
    }

    auto maintenance = std::move(*maintenanceRes);
    auto backfillRes = Result<AudioIdentityIndexResult>{};
    auto exceptionPtr = std::exception_ptr{};
    bool cancelledByException = false;

    try
    {
      co_await _implPtr->asyncRuntime.resumeOnWorker(stopToken);
      setCurrentThreadName("AudioBackfill");
      auto commitBatch = makeAudioIdentityCommitBatch(_implPtr->mutationService, maintenance);
      auto progress = makeAudioIdentityProgressReporter(_implPtr->makeProgressPublisher(), std::move(progressCallback));
      auto failure = makeAudioIdentityFailureReporter(std::move(failureCallback));

      // Fingerprinting runs without mutationService writer ownership; each
      // bounded write-back acquires its own maintenance mutation.
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

    maintenance.finish();
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
