// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/RuntimeFatalProbeScenario.h"

#include "lib/audio/NullBackend.h"
#include "lib/library/PhysicalStoreAccess.h"
#include "runtime/library/LibraryMutationService.h"
#include "runtime/playback/PlaybackTransport.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/async/Executor.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/LoopExecutor.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/QueuedExecutorBase.h>
#include <ao/async/Runtime.h>
#include <ao/async/Signal.h>
#include <ao/async/Task.h>
#include <ao/async/TaskFuture.h>
#include <ao/audio/Backend.h>
#include <ao/audio/BackendIds.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/Device.h>
#include <ao/audio/Player.h>
#include <ao/audio/Property.h>
#include <ao/audio/Subscription.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/WritableMusicLibrary.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/PlaybackMode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/ViewState.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/playback/PlaybackCommands.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/utility/Path.h>

#ifdef _WIN32
#include <ao/winui/app/DestructiveLibraryRestart.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

namespace ao::rt::test
{
  namespace
  {
    struct FatalProbeState final
    {
      async::LifetimeScope* scope = nullptr;
      std::atomic_bool* shutdownStarted = nullptr;
      std::atomic_uint32_t* queueWakeCount = nullptr;
      std::atomic_bool* successorLaunched = nullptr;
      PlaybackCommands* playbackCommands = nullptr;
    };

    FatalProbeState& fatalProbeState() noexcept
    {
      static auto state = FatalProbeState{};
      return state;
    }

    bool probeFatalSink(FatalDiagnostic const& /*diagnostic*/)
    {
      auto const& state = fatalProbeState();

      if (state.scope != nullptr)
      {
        std::fputs(state.scope->empty() ? "probe-scope-empty=true\n" : "probe-scope-empty=false\n", stderr);
      }

      if (state.shutdownStarted != nullptr)
      {
        std::fputs(
          state.shutdownStarted->load() ? "probe-shutdown-started=true\n" : "probe-shutdown-started=false\n", stderr);
      }

      if (state.queueWakeCount != nullptr)
      {
        std::fputs(
          state.queueWakeCount->load() == 2 ? "probe-queue-bookkeeping=true\n" : "probe-queue-bookkeeping=false\n",
          stderr);
      }

      if (state.playbackCommands != nullptr)
      {
        auto const rejectedRes = state.playbackCommands->startFromView(kInvalidViewId, kInvalidTrackId);
        auto const closed = !rejectedRes && rejectedRes.error().code == Error::Code::InvalidState;
        std::fputs(closed ? "probe-playback-closed=true\n" : "probe-playback-closed=false\n", stderr);
      }

      if (state.successorLaunched != nullptr)
      {
        std::fputs(
          state.successorLaunched->load() ? "probe-successor-launched=true\n" : "probe-successor-launched=false\n",
          stderr);
      }

      std::fflush(stderr);
      return true;
    }

    class ProbeQueuedExecutor final : public async::QueuedExecutorBase
    {
    public:
      void drain() { drainQueuedTasks(); }
      void finish() { drainQueuedTasksUntilIdle(); }
      std::atomic_uint32_t& wakeCount() noexcept { return _wakeCount; }

    private:
      void wake() noexcept override { ++_wakeCount; }

      std::atomic_uint32_t _wakeCount{0};
    };

    void enqueueFinalDrainContinuation(ProbeQueuedExecutor& executor)
    {
      executor.defer([&executor] { enqueueFinalDrainContinuation(executor); });
    }

    class RejectingDeferExecutor final : public async::QueuedExecutorBase
    {
    public:
      void defer(compat::MoveOnlyFunction<void()> task) override
      {
        if (_rejectNext.exchange(false))
        {
          throw std::runtime_error{"probe defer rejection"};
        }

        QueuedExecutorBase::defer(std::move(task));
      }

      void rejectNext() noexcept { _rejectNext.store(true); }
      void drain() { drainQueuedTasks(); }

    private:
      void wake() noexcept override {}

      std::atomic_bool _rejectNext{false};
    };

    class ImmediateProbeExecutor final : public async::Executor
    {
    public:
      bool isCurrent() const noexcept override { return true; }
      void dispatch(compat::MoveOnlyFunction<void()> task) override { task(); }
      void defer(compat::MoveOnlyFunction<void()> task) override { task(); }
    };

    class RejectingPublicationExecutor final : public async::Executor
    {
    public:
      bool isCurrent() const noexcept override { return false; }
      void dispatch(compat::MoveOnlyFunction<void()> /*task*/) override
      {
        throw std::runtime_error{"probe publication admission rejection"};
      }
      void defer(compat::MoveOnlyFunction<void()> /*task*/) override {}
    };

    class ThrowingVolumeArm final
    {
    public:
      void arm() noexcept { _armed = true; }
      bool consume() noexcept { return std::exchange(_armed, false); }

    private:
      bool _armed = false;
    };

    class ThrowingVolumeBackend final : public audio::NullBackend
    {
    public:
      explicit ThrowingVolumeBackend(ThrowingVolumeArm& arm)
        : _arm{&arm}
      {
      }

      audio::BackendId backendId() const override { return audio::BackendId{"probe_throwing_backend"}; }
      audio::ProfileId profileId() const override { return audio::ProfileId{audio::kProfileShared}; }

      Result<> setProperty(audio::PropertyId const id, audio::PropertyValue const& value) override
      {
        if (id == audio::PropertyId::Volume && _arm->consume())
        {
          throw std::runtime_error{"probe playback command exception"};
        }

        return NullBackend::setProperty(id, value);
      }

    private:
      ThrowingVolumeArm* _arm;
    };

    class ThrowingVolumeProvider final : public audio::BackendProvider
    {
    public:
      explicit ThrowingVolumeProvider(ThrowingVolumeArm& arm)
        : _arm{&arm}
      {
        _status.descriptor.id = audio::BackendId{"probe_throwing_backend"};
        _status.descriptor.supportedProfiles.push_back({.id = audio::kProfileShared});
        _status.devices.push_back(audio::Device{
          .id = audio::DeviceId{"probe_throwing_device"},
          .displayName = "Probe Throwing Device",
          .description = "Fatal probe output",
          .isDefault = false,
          .backendId = audio::BackendId{"probe_throwing_backend"},
        });
      }

      void shutdown() noexcept override {}

      audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        callback(_status.devices);
        return {};
      }

      Status status() const override { return _status; }

      std::unique_ptr<audio::Backend> createBackend([[maybe_unused]] audio::Device const& device,
                                                    [[maybe_unused]] audio::ProfileId const& profile) override
      {
        return std::make_unique<ThrowingVolumeBackend>(*_arm);
      }

      audio::Subscription subscribeGraph([[maybe_unused]] std::string_view routeAnchor,
                                         [[maybe_unused]] OnGraphChangedCallback callback) override
      {
        return {};
      }

    private:
      ThrowingVolumeArm* _arm;
      Status _status;
    };

    struct BlockingTaskState final
    {
      bool waitUntilStarted()
      {
        auto lock = std::unique_lock{mutex};
        return cv.wait_for(lock, std::chrono::seconds{5}, [this] { return started; });
      }

      void markStartedAndWait()
      {
        auto lock = std::unique_lock{mutex};
        started = true;
        cv.notify_all();
        cv.wait(lock, [this] { return released; });
      }

      void release()
      {
        auto const lock = std::scoped_lock{mutex};
        released = true;
        cv.notify_all();
      }

      std::mutex mutex;
      std::condition_variable cv;
      bool started = false;
      bool released = false;
    };

    async::Task<void> failingTask(async::Runtime* const runtime)
    {
      co_await runtime->resumeOnWorker();
      throw std::runtime_error{"probe exception"};
    }

    async::Task<void> failingCancellableTask(async::Runtime* const runtime, std::stop_token const stopToken)
    {
      co_await runtime->resumeOnWorker(stopToken);
      throw std::runtime_error{"probe exception"};
    }

    async::Task<void> failAfterRelease(std::shared_ptr<BlockingTaskState> statePtr, std::stop_token /*stopToken*/)
    {
      statePtr->markStartedAndWait();
      throw std::runtime_error{"probe exception"};
      co_return;
    }

    async::Task<void> cancelAfterRelease(std::shared_ptr<BlockingTaskState> statePtr, std::stop_token const stopToken)
    {
      auto stopCallback = std::stop_callback{stopToken, [statePtr] { statePtr->release(); }};
      statePtr->markStartedAndWait();
      async::throwIfStopRequested(stopToken);
      co_return;
    }

    std::pair<std::string_view, std::string_view> splitScenario(std::string_view const scenario)
    {
      auto const delimiter = scenario.find(':');

      if (delimiter == std::string_view::npos)
      {
        return {scenario, {}};
      }

      return {scenario.substr(0, delimiter), scenario.substr(delimiter + 1U)};
    }

    Result<std::unique_ptr<AppRuntime>> makePlaybackProbeRuntime(std::string_view const scratchName,
                                                                 std::unique_ptr<async::Executor> executorPtr)
    {
      if (scratchName.empty())
      {
        return makeError(Error::Code::InvalidInput, "Playback probe scratch directory is missing");
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto directoryError = std::error_code{};
      std::filesystem::create_directories(scratchPath, directoryError);

      if (directoryError)
      {
        return makeError(Error::Code::IoError, directoryError.message());
      }

      return AppRuntime::create(AppRuntimeDependencies{
        .executorPtr = std::move(executorPtr),
        .musicRoot = scratchPath,
        .databasePath = scratchPath / "db",
        .musicLibraryPinnedMapBytes = std::size_t{16} * 1024U * 1024U,
        .workspaceConfigStorePtr = std::make_unique<ConfigStore>(scratchPath / "workspace.yaml"),
      });
    }

    std::int32_t runYamlExportMissingCoverResource(std::string_view const scratchName)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes =
        library::MusicLibrary::open(scratchPath,
                                    scratchPath / "db",
                                    library::MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto musicLibrary = std::move(*libraryRes);
      auto writableRes = library::WritableMusicLibrary::acquire(musicLibrary);

      if (!writableRes)
      {
        return 3;
      }

      auto transaction = writableRes->writeTransaction();
      auto const resourceBytes = std::array{std::byte{1}, std::byte{2}, std::byte{3}};
      auto resourceIdRes = library::detail::PhysicalStoreAccess::writer(musicLibrary.resources(), transaction)
                             .create(std::span<std::byte const>{resourceBytes});

      if (!resourceIdRes)
      {
        return 3;
      }

      auto track = library::TrackBuilder::makeEmpty();
      track.property().uri("probe.flac");
      track.coverArt().add(PictureType::FrontCover, *resourceIdRes);
      auto createRes =
        transaction.apply([&track](library::LibraryWrite& write)
                          { return write.tracks().create(track, library::FileManifestBuilder::makeEmpty()); });

      if (!createRes ||
          !library::detail::PhysicalStoreAccess::writer(musicLibrary.resources(), transaction).remove(*resourceIdRes) ||
          !transaction.commit())
      {
        return 3;
      }

      std::ignore = LibraryYamlExporter{musicLibrary}.exportToYaml(scratchPath / "probe.yaml", ExportMode::Full);
      return 3;
    }

    enum class PublicationFailurePhase : std::uint8_t
    {
      Admission,
      Replica,
      Observer,
      Completion,
    };

    std::int32_t runLibraryPublicationFailure(std::string_view const scratchName, PublicationFailurePhase const phase)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto const databasePath = scratchPath / "slice-h-publication-library";
      auto libraryRes = library::MusicLibrary::open(
        scratchPath, databasePath, library::MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto musicLibrary = std::move(*libraryRes);
      auto writableRes = library::WritableMusicLibrary::acquire(musicLibrary);

      if (!writableRes)
      {
        return 3;
      }

      auto immediateExecutor = ImmediateProbeExecutor{};
      auto rejectingExecutor = RejectingPublicationExecutor{};
      auto& executor = phase == PublicationFailurePhase::Admission ? static_cast<async::Executor&>(rejectingExecutor)
                                                                   : static_cast<async::Executor&>(immediateExecutor);
      auto transaction = musicLibrary.readTransaction();
      auto changes =
        LibraryChanges{executor, musicLibrary.libraryRevision(transaction), utility::pathToUtf8(databasePath)};
      auto mutationService = LibraryMutationService{executor, std::move(*writableRes), changes};
      auto replicaBinding = changes.bindReplica("ProbeReplica",
                                                [phase](LibraryChangeSet const&)
                                                {
                                                  if (phase == PublicationFailurePhase::Replica)
                                                  {
                                                    throw std::runtime_error{"probe publication replica exception"};
                                                  }
                                                });
      auto observerSubscription = changes.onChanged(
        [phase](LibraryChangeSet const&)
        {
          if (phase == PublicationFailurePhase::Observer)
          {
            throw std::runtime_error{"probe publication observer exception"};
          }
        });
      auto availabilitySubscription = mutationService.onAvailabilityChanged(
        [phase](LibraryAuthoringAvailability const&)
        {
          if (phase == PublicationFailurePhase::Completion)
          {
            throw std::runtime_error{"probe publication completion exception"};
          }
        });
      auto mutationRes = mutationService.beginInteractiveMutation();

      if (!mutationRes)
      {
        return 3;
      }

      std::ignore = mutationRes->execute([](library::LibraryWrite&) -> Result<OperationOutcome<std::uint8_t>>
                                         { return Changed<std::uint8_t>{.value = 1, .changeSet = {}}; });
      return 3;
    }

    std::int32_t runLibraryMutationExecuteContract(std::string_view const scratchName, bool const applyFirst)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes =
        library::MusicLibrary::open(scratchPath,
                                    scratchPath / "db",
                                    library::MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto writableRes = library::WritableMusicLibrary::acquire(*libraryRes);

      if (!writableRes)
      {
        return 3;
      }

      auto executor = ImmediateProbeExecutor{};
      auto transaction = libraryRes->readTransaction();
      auto changes = LibraryChanges{executor, libraryRes->libraryRevision(transaction), "mutation-execute-probe"};
      auto mutationService = LibraryMutationService{executor, std::move(*writableRes), changes};
      auto mutationRes = mutationService.beginInteractiveMutation();

      if (!mutationRes)
      {
        return 3;
      }

      if (applyFirst)
      {
        auto applyRes = mutationRes->apply([](library::LibraryWrite&) -> Result<> { return {}; });

        if (!applyRes)
        {
          return 3;
        }
      }

      std::ignore = mutationRes->execute(
        [applyFirst](library::LibraryWrite&) -> Result<OperationOutcome<std::uint8_t>>
        {
          return Changed<std::uint8_t>{
            .value = 1,
            .changeSet = LibraryChangeSet{.libraryRevision = applyFirst ? 0U : 1U},
          };
        });
      return 3;
    }

    std::int32_t runLibraryPublicationCompletionAckFailure()
    {
      detail::requireMatchingPublicationCompletion(false, 1, 1, "slice-h-publication-library", "ProbeReplica");
      return 3;
    }

    std::int32_t runSpawnLoggedException()
    {
      auto executor = async::LoopExecutor{};
      auto runtime = async::Runtime{executor, 1};
      runtime.spawnLogged(failingTask(&runtime), "runtime spawnLogged probe");
      runtime.join();
      return 3;
    }

    std::int32_t runCancellableException()
    {
      auto executor = async::LoopExecutor{};
      auto runtime = async::Runtime{executor, 1};
      [[maybe_unused]] auto task = runtime.spawnCancellable([&runtime](std::stop_token const stopToken)
                                                            { return failingCancellableTask(&runtime, stopToken); },
                                                            "runtime cancellable probe");
      runtime.join();
      return 3;
    }

    std::int32_t runLifetimeException()
    {
      auto executor = async::LoopExecutor{};
      auto runtime = async::Runtime{executor, 1};
      auto scope = async::LifetimeScope{};
      fatalProbeState().scope = &scope;

      if (!registerFatalSink(probeFatalSink))
      {
        return 3;
      }

      runtime.spawnWithLifetime(
        scope,
        [&runtime](std::stop_token const stopToken) { return failingCancellableTask(&runtime, stopToken); },
        "runtime lifetime probe");
      runtime.join();
      return 3;
    }

    std::int32_t runShutdownException()
    {
      auto executor = async::LoopExecutor{};
      auto runtime = async::Runtime{executor, 1};
      auto scope = async::LifetimeScope{};
      auto statePtr = std::make_shared<BlockingTaskState>();
      auto shutdown = std::atomic_bool{false};
      fatalProbeState().scope = &scope;
      fatalProbeState().shutdownStarted = &shutdown;

      if (!registerFatalSink(probeFatalSink))
      {
        return 3;
      }

      runtime.spawnWithLifetime(
        scope,
        [statePtr](std::stop_token const stopToken) { return failAfterRelease(statePtr, stopToken); },
        "runtime shutdown probe");

      if (!statePtr->waitUntilStarted())
      {
        return 3;
      }

      shutdown.store(true);
      runtime.requestStop();
      statePtr->release();
      runtime.join();
      return 3;
    }

    std::int32_t runShutdownCancellation()
    {
      auto executor = async::LoopExecutor{};
      auto runtime = async::Runtime{executor, 1};
      auto scope = async::LifetimeScope{};
      auto statePtr = std::make_shared<BlockingTaskState>();
      runtime.spawnWithLifetime(
        scope,
        [statePtr](std::stop_token const stopToken) { return cancelAfterRelease(statePtr, stopToken); },
        "runtime shutdown cancellation probe");

      if (!statePtr->waitUntilStarted())
      {
        return 3;
      }

      scope.cancelAll();
      runtime.requestStop();
      runtime.join();
      return scope.empty() ? 0 : 3;
    }

    std::int32_t runQueuedExecutorException()
    {
      auto executor = ProbeQueuedExecutor{};
      fatalProbeState().queueWakeCount = &executor.wakeCount();

      if (!registerFatalSink(probeFatalSink))
      {
        return 3;
      }

      executor.defer([] { throw std::runtime_error{"probe exception"}; });
      executor.defer([] {});
      executor.drain();
      return 3;
    }

    std::int32_t runQueuedExecutorFinalDrainLimit()
    {
      auto executor = ProbeQueuedExecutor{};

      if (!registerFatalSink(probeFatalSink))
      {
        return 3;
      }

      enqueueFinalDrainContinuation(executor);
      executor.finish();
      return 3;
    }

    std::int32_t runPlaybackPublicationAdmissionException(std::string_view const scratchName)
    {
      auto executorPtr = std::make_unique<RejectingDeferExecutor>();
      auto* const executor = executorPtr.get();
      auto runtimeRes = makePlaybackProbeRuntime(scratchName, std::move(executorPtr));

      if (!runtimeRes)
      {
        return 3;
      }

      auto runtimePtr = std::move(*runtimeRes);
      auto arm = ThrowingVolumeArm{};
      executor->rejectNext();
      runtimePtr->addAudioProvider(std::make_unique<ThrowingVolumeProvider>(arm));
      return 3;
    }

    std::int32_t runPlaybackCommandDrainAdmissionException(std::string_view const scratchName)
    {
      auto executorPtr = std::make_unique<RejectingDeferExecutor>();
      auto* const executor = executorPtr.get();
      auto runtimeRes = makePlaybackProbeRuntime(scratchName, std::move(executorPtr));

      if (!runtimeRes)
      {
        return 3;
      }

      auto runtimePtr = std::move(*runtimeRes);
      auto& playback = runtimePtr->playback();
      bool queuedObserverCommand = false;
      auto const subscription = playback.events().onSnapshot(
        [&playback, &queuedObserverCommand](PlaybackSnapshot const& snapshot)
        {
          if (!queuedObserverCommand && snapshot.succession.shuffle == ShuffleMode::On)
          {
            queuedObserverCommand = true;
            playback.commands().setRepeatMode(RepeatMode::All);
          }
        });

      executor->rejectNext();
      playback.commands().setShuffleMode(ShuffleMode::On);
      return 3;
    }

    std::int32_t runPlaybackCommandException(std::string_view const scratchName)
    {
      auto arm = ThrowingVolumeArm{};
      auto executorPtr = std::make_unique<ProbeQueuedExecutor>();
      auto* const executor = executorPtr.get();
      auto runtimeRes = makePlaybackProbeRuntime(scratchName, std::move(executorPtr));

      if (!runtimeRes)
      {
        return 3;
      }

      auto runtimePtr = std::move(*runtimeRes);
      runtimePtr->addAudioProvider(std::make_unique<ThrowingVolumeProvider>(arm));
      executor->drain();
      auto& playback = runtimePtr->playback();
      playback.commands().setOutputDevice(audio::BackendId{"probe_throwing_backend"},
                                          audio::DeviceId{"probe_throwing_device"},
                                          audio::ProfileId{audio::kProfileShared});
      executor->drain();

      fatalProbeState().playbackCommands = &playback.commands();

      if (!registerFatalSink(probeFatalSink))
      {
        return 3;
      }

      arm.arm();
      executor->defer([commands = &playback.commands()] { commands->setVolume(0.25F); });
      executor->drain();
      return 3;
    }

    std::int32_t runPlaybackRevealOffExecutor(std::string_view const scratchName)
    {
      if (scratchName.empty())
      {
        return 3;
      }

      auto const scratchPath = std::filesystem::temp_directory_path() / std::string{scratchName};
      auto libraryRes =
        library::MusicLibrary::open(scratchPath,
                                    scratchPath / "db",
                                    library::MusicLibrary::Options{.pinnedMapBytes = std::size_t{16} * 1024U * 1024U});

      if (!libraryRes)
      {
        return 3;
      }

      auto library = std::move(*libraryRes);
      auto executor = ProbeQueuedExecutor{};
      auto runtime = async::Runtime{executor};
      auto notifications = NotificationService{runtime};
      auto transport = PlaybackTransport{executor, library, notifications, std::make_unique<audio::Player>(runtime)};
      auto worker = std::jthread{[&transport] { transport.revealPlayingTrack(); }};
      worker.join();
      return 3;
    }

    std::int32_t runWorkspaceObservationAdmissionException(std::string_view const scratchName)
    {
      auto executorPtr = std::make_unique<RejectingDeferExecutor>();
      auto* const executor = executorPtr.get();
      auto runtimeRes = makePlaybackProbeRuntime(scratchName, std::move(executorPtr));

      if (!runtimeRes)
      {
        return 3;
      }

      auto runtimePtr = std::move(*runtimeRes);
      auto preset = CustomTrackPresentationPreset{};
      preset.label = "probe";
      preset.spec.id = "probe";
      executor->rejectNext();
      std::ignore = runtimePtr->workspace().addCustomPreset(preset);
      return 3;
    }

    std::optional<ViewId> createProbeView(AppRuntime& runtime)
    {
      auto viewRes = runtime.workspace().navigate(NavigationRequest{.target = GlobalViewKind::AllTracks});

      if (!viewRes)
      {
        return std::nullopt;
      }

      return *viewRes;
    }

    std::int32_t runViewProjectionObservationAdmissionException(std::string_view const scratchName)
    {
      auto executorPtr = std::make_unique<RejectingDeferExecutor>();
      auto* const executor = executorPtr.get();
      auto runtimeRes = makePlaybackProbeRuntime(scratchName, std::move(executorPtr));

      if (!runtimeRes)
      {
        return 3;
      }

      auto runtimePtr = std::move(*runtimeRes);
      auto const optViewId = createProbeView(*runtimePtr);

      if (!optViewId)
      {
        return 3;
      }

      executor->rejectNext();
      std::ignore = runtimePtr->views().setFilter(*optViewId, "$year > 2000");
      return 3;
    }

    std::int32_t runViewPresentationObservationAdmissionException(std::string_view const scratchName)
    {
      auto executorPtr = std::make_unique<RejectingDeferExecutor>();
      auto* const executor = executorPtr.get();
      auto runtimeRes = makePlaybackProbeRuntime(scratchName, std::move(executorPtr));

      if (!runtimeRes)
      {
        return 3;
      }

      auto runtimePtr = std::move(*runtimeRes);
      auto const optViewId = createProbeView(*runtimePtr);

      if (!optViewId)
      {
        return 3;
      }

      auto presentation = defaultTrackPresentationSpec();
      presentation.id.clear();
      presentation.groupBy = TrackGroupKey::Artist;
      executor->rejectNext();
      std::ignore = runtimePtr->views().setPresentation(*optViewId, presentation);
      return 3;
    }

    std::int32_t runSignalObserverException()
    {
      auto signal = async::Signal<>{};
      [[maybe_unused]] auto subscription = signal.connect([] { throw std::runtime_error{"probe exception"}; });
      signal.emit();
      return 3;
    }

    std::int32_t runTaskFutureMissingResult()
    {
      auto promise = std::promise<std::optional<std::int32_t>>{};
      auto future = async::TaskFuture<std::int32_t>{promise.get_future()};
      promise.set_value(std::nullopt);
      std::ignore = future.get();
      return 3;
    }

#ifdef _WIN32
    std::int32_t runDestructiveRestartReleaseException()
    {
      auto launched = std::atomic_bool{false};
      fatalProbeState().successorLaunched = &launched;

      if (!registerFatalSink(probeFatalSink))
      {
        return 3;
      }

      std::ignore = winui::executeDestructiveLibraryRestart({
        .prepareActiveGraph = [] -> Result<> { return {}; },
        .releaseActiveGraph = [] { throw std::runtime_error{"probe exception"}; },
        .launchSuccessor = [&launched] -> Result<>
        {
          launched.store(true);
          return {};
        },
        .reportPreparationFailure = {},
        .reportLaunchFailure = {},
        .exitProcess = {},
      });
      return 3;
    }

    std::int32_t runDestructiveRestartLaunchException()
    {
      std::ignore = winui::executeDestructiveLibraryRestart({
        .prepareActiveGraph = [] -> Result<> { return {}; },
        .releaseActiveGraph = [] {},
        .launchSuccessor = [] -> Result<> { throw std::runtime_error{"probe exception"}; },
        .reportPreparationFailure = {},
        .reportLaunchFailure = {},
        .exitProcess = {},
      });
      return 3;
    }
#endif
  } // namespace

  std::int32_t runRuntimeFatalProbeScenario(std::string_view const scenario)
  {
    auto const [name, scratchName] = splitScenario(scenario);

    if (name == "yaml-export-missing-cover-resource")
    {
      return runYamlExportMissingCoverResource(scratchName);
    }

    if (name == "library-publication-admission-exception")
    {
      return runLibraryPublicationFailure(scratchName, PublicationFailurePhase::Admission);
    }

    if (name == "library-publication-replica-exception")
    {
      return runLibraryPublicationFailure(scratchName, PublicationFailurePhase::Replica);
    }

    if (name == "library-publication-observer-exception")
    {
      return runLibraryPublicationFailure(scratchName, PublicationFailurePhase::Observer);
    }

    if (name == "library-publication-completion-exception")
    {
      return runLibraryPublicationFailure(scratchName, PublicationFailurePhase::Completion);
    }

    if (name == "library-publication-completion-ack-invariant")
    {
      return runLibraryPublicationCompletionAckFailure();
    }

    if (name == "library-mutation-execute-after-apply")
    {
      return runLibraryMutationExecuteContract(scratchName, true);
    }

    if (name == "library-mutation-prestamped-changeset")
    {
      return runLibraryMutationExecuteContract(scratchName, false);
    }

    if (name == "runtime-spawn-logged-exception")
    {
      return runSpawnLoggedException();
    }

    if (name == "runtime-cancellable-exception")
    {
      return runCancellableException();
    }

    if (name == "runtime-lifetime-exception")
    {
      return runLifetimeException();
    }

    if (name == "runtime-shutdown-exception")
    {
      return runShutdownException();
    }

    if (name == "runtime-shutdown-cancellation")
    {
      return runShutdownCancellation();
    }

    if (name == "queued-executor-callback-exception")
    {
      return runQueuedExecutorException();
    }

    if (name == "queued-executor-final-drain-limit")
    {
      return runQueuedExecutorFinalDrainLimit();
    }

    if (name == "playback-publication-admission-exception")
    {
      return runPlaybackPublicationAdmissionException(scratchName);
    }

    if (name == "playback-command-drain-admission-exception")
    {
      return runPlaybackCommandDrainAdmissionException(scratchName);
    }

    if (name == "playback-command-exception")
    {
      return runPlaybackCommandException(scratchName);
    }

    if (name == "playback-reveal-off-executor")
    {
      return runPlaybackRevealOffExecutor(scratchName);
    }

    if (name == "workspace-observation-admission-exception")
    {
      return runWorkspaceObservationAdmissionException(scratchName);
    }

    if (name == "view-projection-observation-admission-exception")
    {
      return runViewProjectionObservationAdmissionException(scratchName);
    }

    if (name == "view-presentation-observation-admission-exception")
    {
      return runViewPresentationObservationAdmissionException(scratchName);
    }

    if (name == "signal-observer-exception")
    {
      return runSignalObserverException();
    }

    if (name == "task-future-missing-result")
    {
      return runTaskFutureMissingResult();
    }

#ifdef _WIN32

    if (name == "destructive-restart-release-exception")
    {
      return runDestructiveRestartReleaseException();
    }

    if (name == "destructive-restart-launch-exception")
    {
      return runDestructiveRestartLaunchException();
    }

#endif

    return 2;
  }
} // namespace ao::rt::test
