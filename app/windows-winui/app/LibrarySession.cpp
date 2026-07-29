// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/LibrarySession.h"

#include "app/DispatcherQueueExecutor.h"
#include "platform/WindowsStringResources.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/audio/BackendConfig.h>
#include <ao/audio/Transport.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/playback/PlaybackSnapshot.h>
#include <ao/uimodel/layout/shell/WindowsDesktopSettingsYamlSchema.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceLifecycle.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h>
#include <ao/uimodel/library/task/LibraryScanWorkflow.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/utility/Path.h>

#if AOBUS_HAS_WASAPI
#include <ao/audio/backend/WasapiProvider.h>
#endif

#include <algorithm>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::winui
{
  namespace
  {
    void quarantineRuntime(std::shared_ptr<rt::AppRuntime> runtimePtr) noexcept
    {
      if (runtimePtr == nullptr)
      {
        return;
      }

      // Dispatcher shutdown can reject the retirement hop while the runtime's
      // own coroutine is still unwinding. Leak the last reference instead of
      // risking self-destruction on that stack. Allocation failure has no safe
      // recovery here, so this noexcept boundary intentionally terminates.
      // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,bugprone-unhandled-exception-at-new)
      std::ignore = new std::shared_ptr<rt::AppRuntime>{std::move(runtimePtr)};
    }

    bool activeTransport(audio::Transport const transport) noexcept
    {
      return transport == audio::Transport::Opening || transport == audio::Transport::Buffering ||
             transport == audio::Transport::Playing || transport == audio::Transport::Seeking ||
             transport == audio::Transport::Paused;
    }

    bool sameDirectory(std::filesystem::path const& left, std::filesystem::path const& right) noexcept
    {
      auto error = std::error_code{};
      auto const equivalent = std::filesystem::equivalent(left, right, error);
      return !error ? equivalent : left.lexically_normal() == right.lexically_normal();
    }

    struct RuntimeRetirement final
    {
      void retire() noexcept
      {
        runtimePtr.reset();

        if (!afterRelease)
        {
          return;
        }

        try
        {
          auto callback = std::move(afterRelease);
          callback();
        }
        catch (...)
        {
          APP_LOG_ERROR("LibrarySession: candidate retirement callback failed");
        }
      }

      std::shared_ptr<rt::AppRuntime> runtimePtr;
      std::move_only_function<void()> afterRelease;
    };

    void deferRuntimeRelease(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& dispatcher,
                             std::shared_ptr<rt::AppRuntime> runtimePtr,
                             std::move_only_function<void()> afterRelease = {}) noexcept
    {
      if (runtimePtr == nullptr)
      {
        return;
      }

      // The candidate's root coroutine completes on its worker pool. Queue the
      // last owning reference so AppRuntime and Player always shut down on the
      // UI executor after the current coroutine publication has unwound.
      auto retirementPtr = std::shared_ptr<RuntimeRetirement>{};

      try
      {
        retirementPtr = std::make_shared<RuntimeRetirement>();
      }
      catch (...)
      {
        // Without a retirement record the owner-affine callback cannot run
        // safely; session teardown owns any remaining candidate bookkeeping.
        quarantineRuntime(std::move(runtimePtr));
        return;
      }

      retirementPtr->runtimePtr = std::move(runtimePtr);
      retirementPtr->afterRelease.swap(afterRelease);

      bool queued = false;

      try
      {
        queued = dispatcher.TryEnqueue([retirementPtr] { retirementPtr->retire(); });
      }
      catch (...)
      {
        queued = false;
      }

      if (!queued)
      {
        APP_LOG_CRITICAL("LibrarySession: UI dispatcher rejected candidate runtime retirement");
        // The callback is owner-affine and cannot run after dispatcher
        // shutdown. Keep the runtime alive and let session teardown discard
        // any remaining candidate bookkeeping.
        quarantineRuntime(std::move(retirementPtr->runtimePtr));
      }
    }

    class DeferredRuntimeOwner final
    {
    public:
      DeferredRuntimeOwner(winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
                           std::shared_ptr<rt::AppRuntime> runtimePtr,
                           std::move_only_function<void()> afterRelease)
        : _dispatcher{std::move(dispatcher)}, _runtimePtr{std::move(runtimePtr)}, _afterRelease{std::move(afterRelease)}
      {
      }

      ~DeferredRuntimeOwner() { deferRuntimeRelease(_dispatcher, std::move(_runtimePtr), std::move(_afterRelease)); }

      DeferredRuntimeOwner(DeferredRuntimeOwner const&) = delete;
      DeferredRuntimeOwner& operator=(DeferredRuntimeOwner const&) = delete;
      DeferredRuntimeOwner(DeferredRuntimeOwner&&) noexcept = default;
      DeferredRuntimeOwner& operator=(DeferredRuntimeOwner&&) = delete;

      std::shared_ptr<rt::AppRuntime> share() const { return _runtimePtr; }

    private:
      winrt::Microsoft::UI::Dispatching::DispatcherQueue _dispatcher{nullptr};
      std::shared_ptr<rt::AppRuntime> _runtimePtr;
      std::move_only_function<void()> _afterRelease;
    };

    void checkpointWorkspace(rt::AppRuntime& runtime, std::string_view const reason) noexcept
    {
      try
      {
        runtime.workspace().saveSession(runtime.workspaceConfigStore());
      }
      catch (std::exception const& error)
      {
        APP_LOG_ERROR("LibrarySession: failed to checkpoint workspace during {}: {}", reason, error.what());
      }
      catch (...)
      {
        APP_LOG_ERROR("LibrarySession: failed to checkpoint workspace during {}: unknown exception", reason);
      }
    }
  } // namespace

  LibrarySession::LibrarySession(std::filesystem::path stateRoot,
                                 winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher)
    : _stateRoot{std::move(stateRoot)}
    , _dispatcher{std::move(dispatcher)}
    , _settingsStorePtr{std::make_unique<rt::ConfigStore>(_stateRoot / "windows-settings.yaml")}
    , _playbackStorePtr{std::make_unique<rt::ConfigStore>(_stateRoot / "windows-playback.yaml")}
  {
    std::filesystem::create_directories(_stateRoot);

    if (auto loaded = _settingsStorePtr->load("desktop", _settings, uimodel::WindowsDesktopSettingsYamlSchema{});
        !loaded && loaded.error().code != Error::Code::NotFound)
    {
      APP_LOG_WARN("LibrarySession: failed to load Windows settings: {}", loaded.error().message);
    }

    if (auto loaded =
          _settingsStorePtr->load("trackView.columnLayouts", _columnLayouts, uimodel::TrackColumnLayoutYamlSchema{});
        !loaded && loaded.error().code != Error::Code::NotFound)
    {
      APP_LOG_WARN("LibrarySession: failed to load Windows column layouts: {}", loaded.error().message);
    }

    if (auto loaded = _settingsStorePtr->load(
          "trackView.presentations", _presentationPreferences, uimodel::ListPresentationPreferenceYamlSchema{});
        !loaded && loaded.error().code != Error::Code::NotFound)
    {
      APP_LOG_WARN("LibrarySession: failed to load Windows presentation preferences: {}", loaded.error().message);
    }

    auto root = utility::pathFromUtf8(_settings.lastLibraryPath);

    if (root.empty() || !std::filesystem::is_directory(root))
    {
      root = _stateRoot / "empty-library";
      std::filesystem::create_directories(root);
    }

    _libraryRuntimePtr = createRuntime(root);
    bindPresentationPreferenceLifecycle();
    bindPlaybackRuntime(_libraryRuntimePtr);
  }

  LibrarySession::~LibrarySession()
  {
    _callbackLifetimePtr.reset();
    _libraryTask.reset();
    _retainedPlaybackSub.reset();
    _callbacks = {};

    if (_libraryRuntimePtr != nullptr)
    {
      checkpointWorkspace(*_libraryRuntimePtr, "session teardown");
    }

    _playbackCommandsPtr.reset();
    _presentationPreferenceLifecyclePtr.reset();
    _playbackRuntimePtr.reset();
    _libraryRuntimePtr.reset();
  }

  rt::AppRuntime& LibrarySession::libraryRuntime() const noexcept
  {
    return *_libraryRuntimePtr;
  }

  rt::AppRuntime& LibrarySession::playbackRuntime() const noexcept
  {
    return *_playbackRuntimePtr;
  }

  uimodel::PlaybackCommandSurface& LibrarySession::playbackCommands() const noexcept
  {
    return *_playbackCommandsPtr;
  }

  Result<> LibrarySession::saveSettings()
  {
    checkpointWorkspace(*_libraryRuntimePtr, "settings save");
    return _settingsStorePtr->saveTogether(
      rt::configWrite("desktop", _settings, uimodel::WindowsDesktopSettingsYamlSchema{}),
      rt::configWrite("trackView.columnLayouts", _columnLayouts, uimodel::TrackColumnLayoutYamlSchema{}),
      rt::configWrite(
        "trackView.presentations", _presentationPreferences, uimodel::ListPresentationPreferenceYamlSchema{}));
  }

  void LibrarySession::setCallbacks(LibrarySessionCallbacks callbacks)
  {
    _callbacks = std::move(callbacks);
  }

  std::shared_ptr<rt::AppRuntime> LibrarySession::createRuntime(std::filesystem::path const& root)
  {
    auto const paths = rt::LibraryPaths{root};
    auto workspaceStorePtr = std::make_unique<rt::ConfigStore>(paths.databasePath() / "workspace.yaml");
    auto executorPtr = std::make_unique<DispatcherQueueExecutor>(_dispatcher);
    auto runtimePtr = std::make_shared<rt::AppRuntime>(
      rt::AppRuntimeDependencies{.executorPtr = std::move(executorPtr),
                                 .musicRoot = root,
                                 .databasePath = paths.databasePath(),
                                 .workspaceConfigStorePtr = std::move(workspaceStorePtr),
                                 .playbackSessionConfigStore = _playbackStorePtr.get(),
                                 .asyncExceptionHandler = rt::Log::asyncExceptionHandler()});
#if AOBUS_HAS_WASAPI
    runtimePtr->addAudioProvider(std::make_unique<audio::backend::WasapiProvider>());
#endif
    runtimePtr->reloadAllTracks();

    if (auto const restored = runtimePtr->workspace().restoreSession(runtimePtr->workspaceConfigStore()); !restored)
    {
      APP_LOG_WARN("LibrarySession: failed to restore workspace for '{}': {}",
                   utility::pathToUtf8(root),
                   restored.error().message);
    }

    return runtimePtr;
  }

  void LibrarySession::bindPresentationPreferenceLifecycle()
  {
    _presentationPreferenceLifecyclePtr.reset();
    _presentationPreferenceLifecyclePtr = std::make_unique<uimodel::ListPresentationPreferenceLifecycle>(
      _presentationPreferences.presentations,
      _libraryRuntimePtr->library().changes(),
      [this](ListId const) noexcept
      {
        try
        {
          auto const saved = _settingsStorePtr->save(
            "trackView.presentations", _presentationPreferences, uimodel::ListPresentationPreferenceYamlSchema{});

          if (!saved)
          {
            APP_LOG_WARN(
              "LibrarySession: failed to persist deleted List preference cleanup: {}", saved.error().message);
          }
        }
        catch (std::exception const& error)
        {
          APP_LOG_WARN("LibrarySession: deleted List preference cleanup failed: {}", error.what());
        }
        catch (...)
        {
          APP_LOG_WARN("LibrarySession: deleted List preference cleanup failed");
        }
      });
  }

  void LibrarySession::bindPlaybackRuntime(std::shared_ptr<rt::AppRuntime> runtimePtr)
  {
    if (_playbackRuntimePtr && _callbacks.onPlaybackChanging)
    {
      _callbacks.onPlaybackChanging();
    }

    _retainedPlaybackSub.reset();
    _playbackCommandsPtr.reset();
    _playbackRuntimePtr = std::move(runtimePtr);
    _playbackCommandsPtr = std::make_unique<uimodel::PlaybackCommandSurface>(
      _playbackRuntimePtr->playback(), [this] { requestPlaySelection(); });
    _adoptScheduled = false;

    if (_callbacks.onPlaybackChanged)
    {
      _callbacks.onPlaybackChanged();
    }
  }

  void LibrarySession::openLibrary(std::filesystem::path root)
  {
    root = std::filesystem::absolute(root).lexically_normal();
    auto const hasExistingDatabase = rt::LibraryPaths{root}.hasExistingDatabase();

    if (sameDirectory(root, _libraryRuntimePtr->musicRoot()))
    {
      if (!hasExistingDatabase)
      {
        rescan();
      }
      else if (_callbacks.onStatus)
      {
        _callbacks.onStatus(formatResource("LibraryReadyFormat", utility::pathToUtf8(root)));
      }

      return;
    }

    if (candidateRootInUse(root))
    {
      if (_callbacks.onStatus)
      {
        _callbacks.onStatus(resourceString("PreparingLibrary"));
      }

      return;
    }

    cancelLibraryOperation();
    auto const mode =
      hasExistingDatabase ? LibraryPreparationMode::OpenExisting : LibraryPreparationMode::ScanCandidate;
    prepareAndSwap(std::move(root), mode);
  }

  void LibrarySession::rescan()
  {
    if (_operationActive && _optOperationRoot && sameDirectory(*_optOperationRoot, _libraryRuntimePtr->musicRoot()))
    {
      if (_callbacks.onStatus)
      {
        _callbacks.onStatus(resourceString("RescanningLibrary"));
      }

      return;
    }

    cancelLibraryOperation();
    // A same-root candidate would open the same LMDB environment twice in one
    // process. The shared scan workflow already owns transactional apply, so
    // rescan in place and publish a fresh projection only after it completes.
    prepareAndSwap(_libraryRuntimePtr->musicRoot(), LibraryPreparationMode::RescanActive);
  }

  void LibrarySession::cancelLibraryOperation()
  {
    auto const operationWasActive = _operationActive;
    _libraryTask.reset();
    ++_operationGeneration;
    _optOperationRoot.reset();
    _operationActive = false;

    if (operationWasActive && _callbacks.onLibraryTaskRuntimeChanged)
    {
      _callbacks.onLibraryTaskRuntimeChanged(_libraryRuntimePtr);
    }
  }

  bool LibrarySession::candidateRootInUse(std::filesystem::path const& root) const
  {
    return std::ranges::any_of(
      _candidateRoots, [&root](std::filesystem::path const& candidate) { return sameDirectory(root, candidate); });
  }

  void LibrarySession::releaseCandidateRoot(std::filesystem::path const& root)
  {
    std::erase_if(
      _candidateRoots, [&root](std::filesystem::path const& candidate) { return sameDirectory(root, candidate); });
  }

  void LibrarySession::prepareAndSwap(std::filesystem::path root, LibraryPreparationMode const mode)
  {
    auto const replaceLibrary = mode != LibraryPreparationMode::RescanActive;
    auto const operationGeneration = ++_operationGeneration;
    auto const registeredRoot = root;
    _optOperationRoot = root;
    _operationActive = true;

    try
    {
      if (replaceLibrary)
      {
        _candidateRoots.push_back(root);
      }

      auto candidatePtr = replaceLibrary ? createRuntime(root) : _libraryRuntimePtr;

      if (_callbacks.onLibraryTaskRuntimeChanged)
      {
        _callbacks.onLibraryTaskRuntimeChanged(candidatePtr);
      }

      auto const lifetimePtr = std::weak_ptr<CallbackLifetime>{_callbackLifetimePtr};
      auto const dispatcher = _dispatcher;
      _libraryTask = candidatePtr->async().spawnCancellable(
        [owner = this,
         lifetimePtr,
         dispatcher,
         candidateOwner = DeferredRuntimeOwner{dispatcher,
                                               candidatePtr,
                                               [owner = this, lifetimePtr, registeredRoot, replaceLibrary]
                                               {
                                                 if (replaceLibrary && !lifetimePtr.expired())
                                                 {
                                                   owner->releaseCandidateRoot(registeredRoot);
                                                 }
                                               }},
         root = std::move(root),
         mode,
         operationGeneration](std::stop_token const stopToken) mutable
        {
          return prepareAndSwapWorkflow(owner,
                                        lifetimePtr,
                                        dispatcher,
                                        candidateOwner.share(),
                                        std::move(root),
                                        mode,
                                        operationGeneration,
                                        stopToken);
        });
    }
    catch (std::exception const& error)
    {
      if (replaceLibrary)
      {
        releaseCandidateRoot(registeredRoot);
      }

      if (_operationGeneration == operationGeneration)
      {
        _optOperationRoot.reset();
        _operationActive = false;
      }

      if (_callbacks.onLibraryTaskRuntimeChanged)
      {
        _callbacks.onLibraryTaskRuntimeChanged(_libraryRuntimePtr);
      }

      _libraryRuntimePtr->notifications().post(rt::NotificationSeverity::Error,
                                               formatResource("ErrorFormat", error.what()),
                                               rt::NotificationLifetime::history());

      if (_callbacks.onFailure)
      {
        _callbacks.onFailure(Error{.code = Error::Code::Generic, .message = error.what()});
      }
    }
  }

  bool LibrarySession::workflowRetired(LibrarySession const* const owner,
                                       std::weak_ptr<CallbackLifetime> const& lifetimePtr,
                                       std::uint64_t const operationGeneration) noexcept
  {
    return lifetimePtr.expired() || owner->_operationGeneration != operationGeneration;
  }

  void LibrarySession::completeLibraryPreparation(LibrarySession* const owner,
                                                  std::shared_ptr<rt::AppRuntime>& candidatePtr,
                                                  std::filesystem::path const& root,
                                                  bool const replaceLibrary,
                                                  std::uint64_t const operationGeneration)
  {
    if (replaceLibrary)
    {
      checkpointWorkspace(*owner->_libraryRuntimePtr, "library replacement");

      if (owner->_callbacks.onLibraryChanging)
      {
        owner->_callbacks.onLibraryChanging();
      }

      owner->_libraryRuntimePtr = std::move(candidatePtr);
      owner->bindPresentationPreferenceLifecycle();
      owner->_settings.lastLibraryPath = utility::pathToUtf8(root);

      if (auto const saved = owner->saveSettings(); !saved)
      {
        APP_LOG_WARN("LibrarySession: failed to persist selected library: {}", saved.error().message);
      }

      if (owner->_callbacks.onLibraryChanged)
      {
        owner->_callbacks.onLibraryChanged();
      }

      owner->retainPlaybackUntilIdle();
    }
    else if (owner->_callbacks.onLibraryChanged)
    {
      owner->_callbacks.onLibraryChanged();
    }

    if (owner->_callbacks.onStatus)
    {
      owner->_callbacks.onStatus(ao::winui::formatResource("LibraryReadyFormat", utility::pathToUtf8(root)));
    }

    if (owner->_operationGeneration == operationGeneration)
    {
      owner->_optOperationRoot.reset();
      owner->_operationActive = false;
    }
  }

  void LibrarySession::completeFailedLibraryPreparation(
    LibrarySession* const owner,
    winrt::Microsoft::UI::Dispatching::DispatcherQueue const& dispatcher,
    std::shared_ptr<rt::AppRuntime> candidatePtr,
    bool const cancelled,
    std::optional<Error> const& optFailure)
  {
    if (owner->_callbacks.onLibraryTaskRuntimeChanged)
    {
      owner->_callbacks.onLibraryTaskRuntimeChanged(owner->_libraryRuntimePtr);
    }

    deferRuntimeRelease(dispatcher, std::move(candidatePtr));
    owner->_optOperationRoot.reset();
    owner->_operationActive = false;

    if (cancelled)
    {
      owner->_libraryRuntimePtr->notifications().post(rt::NotificationSeverity::Info,
                                                      ao::winui::resourceString("LibraryOperationCancelled"),
                                                      rt::NotificationLifetime::transient());

      if (owner->_callbacks.onStatus)
      {
        owner->_callbacks.onStatus(ao::winui::resourceString("LibraryOperationCancelled"));
      }

      return;
    }

    if (!optFailure)
    {
      return;
    }

    owner->_libraryRuntimePtr->notifications().post(rt::NotificationSeverity::Error,
                                                    ao::winui::formatResource("ErrorFormat", optFailure->message),
                                                    rt::NotificationLifetime::history());

    if (owner->_callbacks.onFailure)
    {
      owner->_callbacks.onFailure(*optFailure);
    }
  }

  async::Task<void> LibrarySession::prepareAndSwapWorkflow(
    LibrarySession* const owner,
    std::weak_ptr<CallbackLifetime> const lifetimePtr,
    winrt::Microsoft::UI::Dispatching::DispatcherQueue const dispatcher,
    std::shared_ptr<rt::AppRuntime> candidatePtr,
    std::filesystem::path root,
    LibraryPreparationMode const mode,
    std::uint64_t const operationGeneration,
    std::stop_token const stopToken)
  {
    auto const replaceLibrary = mode != LibraryPreparationMode::RescanActive;
    auto const scanLibrary = mode != LibraryPreparationMode::OpenExisting;
    auto* const callbackRuntime = candidatePtr.get();
    auto optFailure = std::optional<Error>{};
    bool cancelled = false;

    try
    {
      co_await callbackRuntime->async().resumeOnCallbackExecutor(stopToken);

      if (workflowRetired(owner, lifetimePtr, operationGeneration))
      {
        deferRuntimeRelease(dispatcher, std::move(candidatePtr));
        co_return;
      }

      if (owner->_callbacks.onStatus)
      {
        owner->_callbacks.onStatus(ao::winui::resourceString(
          mode == LibraryPreparationMode::RescanActive ? "RescanningLibrary" : "PreparingLibrary"));
      }

      if (scanLibrary)
      {
        auto scan = co_await uimodel::runLibraryScanWorkflow(
          &candidatePtr->library().taskService(), uimodel::LibraryScanMode::Eager, stopToken);
        async::throwIfStopRequested(stopToken);

        if (workflowRetired(owner, lifetimePtr, operationGeneration))
        {
          deferRuntimeRelease(dispatcher, std::move(candidatePtr));
          co_return;
        }

        if (!scan)
        {
          optFailure = scan.error().error;
        }
        else if (scan->disposition == uimodel::LibraryScanPlanDisposition::ErrorsOnly)
        {
          optFailure = Error{
            .code = Error::Code::FormatRejected,
            .message = ao::winui::formatResource(
              scan->summary.errorCount == 1 ? "LibraryScanUnreadableOneFormat" : "LibraryScanUnreadableManyFormat",
              scan->summary.errorCount),
          };
        }
        else
        {
          candidatePtr->reloadAllTracks();
        }
      }

      if (!optFailure)
      {
        completeLibraryPreparation(owner, candidatePtr, root, replaceLibrary, operationGeneration);
      }
    }
    catch (std::exception const& error)
    {
      if (async::isOperationCancelled(error))
      {
        cancelled = true;
      }
      else
      {
        optFailure = Error{.code = Error::Code::Generic, .message = error.what()};
      }
    }

    if (!cancelled && !optFailure)
    {
      co_return;
    }

    // Cancellation can win before the workflow's initial UI hop. Return to the
    // callback executor before touching shell callbacks or retiring the runtime.
    co_await callbackRuntime->async().resumeOnCallbackExecutor();

    // Resetting the operation handle requests stop without joining the
    // candidate runtime. Owner teardown invalidates the lifetime token before
    // making that request; ordinary supersession leaves the owner available so
    // its candidate-root registration can be retired on the UI executor.
    if (lifetimePtr.expired())
    {
      deferRuntimeRelease(dispatcher, std::move(candidatePtr));
      co_return;
    }

    if (stopToken.stop_requested() || owner->_operationGeneration != operationGeneration)
    {
      deferRuntimeRelease(dispatcher, std::move(candidatePtr));
      co_return;
    }

    completeFailedLibraryPreparation(owner, dispatcher, std::move(candidatePtr), cancelled, optFailure);
  }

  void LibrarySession::retainPlaybackUntilIdle()
  {
    if (_playbackRuntimePtr == _libraryRuntimePtr)
    {
      return;
    }

    if (!activeTransport(_playbackRuntimePtr->playback().snapshot().transport.transport))
    {
      bindPlaybackRuntime(_libraryRuntimePtr);
      return;
    }

    auto const lifetimePtr = std::weak_ptr<CallbackLifetime>{_callbackLifetimePtr};
    _retainedPlaybackSub = _playbackRuntimePtr->playback().events().onSnapshot(
      [this, lifetimePtr](rt::PlaybackSnapshot const& snapshot) noexcept
      {
        if (lifetimePtr.expired())
        {
          return;
        }

        if (!activeTransport(snapshot.transport.transport))
        {
          scheduleAdoptLibraryPlayback();
        }
      });
  }

  void LibrarySession::scheduleAdoptLibraryPlayback()
  {
    if (_adoptScheduled)
    {
      return;
    }

    _adoptScheduled = true;
    auto const lifetimePtr = std::weak_ptr<CallbackLifetime>{_callbackLifetimePtr};
    auto const queued = _dispatcher.TryEnqueue(
      [this, lifetimePtr]
      {
        if (lifetimePtr.expired())
        {
          return;
        }

        if (_playbackRuntimePtr != _libraryRuntimePtr)
        {
          bindPlaybackRuntime(_libraryRuntimePtr);
        }

        _adoptScheduled = false;
      });

    if (!queued)
    {
      _adoptScheduled = false;
    }
  }

  void LibrarySession::requestPlaySelection()
  {
    if (_playbackRuntimePtr == _libraryRuntimePtr)
    {
      std::ignore = _libraryRuntimePtr->playSelectionInFocusedView();
      return;
    }

    auto const lifetimePtr = std::weak_ptr<CallbackLifetime>{_callbackLifetimePtr};
    auto const queued = _dispatcher.TryEnqueue(
      [this, lifetimePtr]
      {
        if (lifetimePtr.expired())
        {
          return;
        }

        bindPlaybackRuntime(_libraryRuntimePtr);
        std::ignore = _libraryRuntimePtr->playSelectionInFocusedView();
      });

    if (!queued)
    {
      APP_LOG_WARN("LibrarySession: UI dispatcher rejected playback session adoption");
    }
  }

  Result<> LibrarySession::playTrack(rt::ViewId const viewId, TrackId const trackId)
  {
    if (_playbackRuntimePtr != _libraryRuntimePtr)
    {
      bindPlaybackRuntime(_libraryRuntimePtr);
    }

    if (auto selected = _libraryRuntimePtr->views().setSelection(viewId, {trackId}); !selected)
    {
      return selected;
    }

    if (auto focused = _libraryRuntimePtr->workspace().focusView(viewId); !focused)
    {
      return focused;
    }

    return _libraryRuntimePtr->playback().commands().startFromView(viewId, trackId);
  }
} // namespace ao::winui
