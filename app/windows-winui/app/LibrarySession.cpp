// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/LibrarySession.h"

#include "app/DispatcherQueueExecutor.h"
#include "platform/StringResources.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/desktop/LibraryStartupPlanner.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ConfigStore.h>
#include <ao/rt/Log.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/NotificationState.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/library/LibraryTaskService.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/input/KeymapStore.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceLifecycle.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/presentation/TrackPresentationRecommender.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
#include <ao/uimodel/library/task/LibraryScanWorkflow.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>
#include <ao/utility/Path.h>
#include <ao/utility/PlatformDirectories.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/WinUiErrorBoundary.h>
#include <ao/winui/app/DesktopOutputSelection.h>
#include <ao/winui/app/SelectedRootCommit.h>

#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace ao::winui
{
  namespace
  {
    using LibraryScanResult = std::expected<uimodel::LibraryScanWorkflowResult, uimodel::LibraryScanWorkflowFailure>;
    using PresentLibraryScan = compat::MoveOnlyFunction<void(LibraryScanResult)>;

    void checkpointWorkspaceBestEffort(rt::AppRuntime& runtime) noexcept
    {
      try
      {
        runtime.workspace().saveSession(runtime.workspaceConfigStore());
      }
      catch (std::exception const& error)
      {
        AO_AUDITED_CATCH(SafeCleanup);
        logWinUiCritical("LibrarySession teardown checkpoint", error.what());
      }
      catch (...)
      {
        AO_AUDITED_CATCH(SafeCleanup);
        logWinUiCritical("LibrarySession teardown checkpoint", "Unknown exception");
      }
    }

    async::Task<void> runActiveScan(rt::LibraryTaskService* const service,
                                    PresentLibraryScan present,
                                    std::stop_token const stopToken)
    {
      auto result = co_await uimodel::runLibraryScanWorkflow(service, uimodel::LibraryScanMode::Eager, stopToken);
      async::throwIfStopRequested(stopToken);
      present(std::move(result));
    }
  } // namespace

  Result<std::unique_ptr<LibrarySession>> LibrarySession::create(
    std::filesystem::path stateRoot,
    winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
    uimodel::PresentationTextCatalog textCatalog,
    rt::TextOrderingPolicy const& textOrderingPolicy,
    std::optional<desktop::LibrarySwitchRequest> optSuccessorRequest)
  {
    auto sessionPtr = std::unique_ptr<LibrarySession>{
      new LibrarySession{std::move(stateRoot), std::move(dispatcher), std::move(textCatalog), textOrderingPolicy}};

    if (auto initializedRes = sessionPtr->initialize(std::move(optSuccessorRequest)); !initializedRes)
    {
      return std::unexpected{initializedRes.error()};
    }

    return sessionPtr;
  }

  LibrarySession::LibrarySession(std::filesystem::path stateRoot,
                                 winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
                                 uimodel::PresentationTextCatalog textCatalog,
                                 rt::TextOrderingPolicy const& textOrderingPolicy)
    : _stateRoot{std::move(stateRoot)}
    , _dispatcher{std::move(dispatcher)}
    , _textCatalog{std::move(textCatalog)}
    , _textOrderingPolicy{textOrderingPolicy}
    , _settingsStorePtr{std::make_unique<rt::ConfigStore>(_stateRoot / "windows-settings.yaml")}
    , _playbackStorePtr{std::make_unique<rt::ConfigStore>(_stateRoot / "windows-playback.yaml")}
  {
  }

  Result<> LibrarySession::initialize(std::optional<desktop::LibrarySwitchRequest> optSuccessorRequest)
  {
    auto directoryEc = std::error_code{};
    std::filesystem::create_directories(_stateRoot, directoryEc);

    if (directoryEc)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to create the WinUI state directory: {}", directoryEc.message()));
    }

    if (auto loadedRes = _settingsStorePtr->load("desktop", _settings, winui::DesktopSettingsYamlSchema{});
        !loadedRes && loadedRes.error().code != Error::Code::NotFound)
    {
      APP_LOG_WARN("LibrarySession: failed to load Windows settings: {}", loadedRes.error().message);
    }

    if (auto loadedRes =
          _settingsStorePtr->load("trackView.columnLayouts", _columnLayouts, uimodel::TrackColumnLayoutYamlSchema{});
        !loadedRes && loadedRes.error().code != Error::Code::NotFound)
    {
      APP_LOG_WARN("LibrarySession: failed to load Windows column layouts: {}", loadedRes.error().message);
    }

    if (auto loadedRes = _settingsStorePtr->load(
          "trackView.presentations", _presentationPreferences, uimodel::ListPresentationPreferenceYamlSchema{});
        !loadedRes && loadedRes.error().code != Error::Code::NotFound)
    {
      APP_LOG_WARN("LibrarySession: failed to load Windows presentation preferences: {}", loadedRes.error().message);
    }

    // Shortcuts share the settings file: one store, one process, so the whole
    // document is still written as a unit.
    _keymap = uimodel::loadKeymap(*_settingsStorePtr, uimodel::defaultKeymap());

    auto optPersistedRoot = std::optional<std::filesystem::path>{};

    if (!_settings.lastLibraryPath.empty())
    {
      try
      {
        optPersistedRoot = utility::pathFromUtf8(_settings.lastLibraryPath);
      }
      catch (std::filesystem::filesystem_error const&)
      {
        optPersistedRoot.reset();
      }
    }

    auto startupPlanRes = desktop::planLibraryStartup({
      .optSuccessorRequest = std::move(optSuccessorRequest),
      .optPersistedRoot = std::move(optPersistedRoot),
      .emptyLibraryRoot = _stateRoot / "empty-library",
    });

    if (!startupPlanRes)
    {
      return makeError(startupPlanRes.error().code,
                       std::format("Failed to select the startup library: {}", startupPlanRes.error().message));
    }

    if (startupPlanRes->source == desktop::LibraryStartupRootSource::EmptyLibraryFallback)
    {
      std::filesystem::create_directories(startupPlanRes->libraryRoot, directoryEc);

      if (directoryEc)
      {
        return makeError(
          Error::Code::IoError, std::format("Failed to create the fallback library: {}", directoryEc.message()));
      }
    }

    auto root = std::move(startupPlanRes->libraryRoot);
    _optSelectedRootCommit = std::move(startupPlanRes->optSelectedRootCommit);
    _playbackPersistenceAdmission = startupPlanRes->playbackPersistence == desktop::PlaybackPersistenceStartup::Restore
                                      ? PlaybackPersistenceAdmission::Ready
                                      : PlaybackPersistenceAdmission::AwaitingRootCommit;
    _scanAfterOpen = startupPlanRes->scanAfterOpen || !rt::LibraryPaths{root}.hasExistingDatabase();
    auto runtimeRes = createRuntime(root);

    if (!runtimeRes)
    {
      return makeError(
        runtimeRes.error().code, std::format("Failed to open initial library: {}", runtimeRes.error().message));
    }

    _runtimePtr = std::move(*runtimeRes);
    auto& playback = _runtimePtr->playback();
    auto const optOutputSelection =
      resolveDesktopOutputSelectionToRestore(_settings, playback.snapshot().transport.output);

    if (optOutputSelection)
    {
      playback.commands().setOutputDevice(
        optOutputSelection->backendId, optOutputSelection->deviceId, optOutputSelection->profileId);
    }

    if (_playbackPersistenceAdmission == PlaybackPersistenceAdmission::Ready)
    {
      _runtimePtr->startPlaybackSessionPersistence();

      if (auto restoredRes = _runtimePtr->restorePlaybackSession(); !restoredRes)
      {
        APP_LOG_WARN("LibrarySession: failed to restore Windows playback session for '{}': {}",
                     utility::pathToUtf8(root),
                     restoredRes.error().message);
      }
    }

    bindRuntimeServices();
    return {};
  }

  LibrarySession::~LibrarySession()
  {
    shutdown();
  }

  void LibrarySession::shutdown() noexcept
  {
    if (_shutdown)
    {
      return;
    }

    _shutdown = true;

    // Invalidate every callback target before requesting cancellation. A task
    // or queued dispatcher continuation may still exist until the runtime joins,
    // but its weak lifetime token must already be dead.
    _callbackLifetimePtr.reset();
    _callbacks = {};
    _operationActive = false;
    _operationStatusKey = {};

    _libraryTask.reset();

    if (_runtimePtr != nullptr)
    {
      checkpointWorkspaceBestEffort(*_runtimePtr);
    }

    // These UI-model owners borrow runtime services. They must disappear while
    // the runtime, stores, and dispatcher are still alive.
    _playbackCommandsPtr.reset();
    _presentationPreferenceLifecyclePtr.reset();
    _presentationCatalogPtr.reset();

    // AppRuntime::shutdown() requests stop and joins worker work before its
    // stores and the session's dispatcher are destroyed.
    _runtimePtr.reset();
  }

  rt::AppRuntime& LibrarySession::runtime() const noexcept
  {
    return *_runtimePtr;
  }

  std::filesystem::path const& LibrarySession::musicRoot() const noexcept
  {
    return _runtimePtr->musicRoot();
  }

  uimodel::PlaybackCommandSurface& LibrarySession::playbackCommands() const noexcept
  {
    return *_playbackCommandsPtr;
  }

  rt::TrackPresentationSpec LibrarySession::presentationForList(ListId const listId) const
  {
    auto preferences = uimodel::ListPresentationPreferenceStore{*_presentationCatalogPtr};
    preferences.setListPresentations(_presentationPreferences.presentations);
    auto context = uimodel::ListPresentationContext{
      .listId = listId,
      .sourceKind = uimodel::ListPresentationSourceKind::AllTracks,
    };

    if (!rt::isVirtualListId(listId))
    {
      if (auto const optNode = _runtimePtr->library().reader().listNode(listId); optNode)
      {
        context.sourceKind = uimodel::ListPresentationSourceKind::SavedList;
        context.listExpression = optNode->expression;
        return preferences.presentationForList(context);
      }
    }

    return preferences.presentationForList(context);
  }

  Result<> LibrarySession::saveSettings()
  {
    if (_shutdown)
    {
      return makeError(Error::Code::InvalidState, "The WinUI library session is shutting down");
    }

    return saveSettingsCandidate(_settings);
  }

  void LibrarySession::setPreferredOutputSelection(audio::OutputDeviceSelection const& selection) noexcept
  {
    std::ignore = rememberDesktopOutputSelection(_settings, selection);
  }

  Result<> LibrarySession::saveSettingsCandidate(DesktopSettings const& settings)
  {
    _runtimePtr->workspace().saveSession(_runtimePtr->workspaceConfigStore());
    return _settingsStorePtr->saveTogether(
      rt::configWrite("desktop", settings, winui::DesktopSettingsYamlSchema{}),
      rt::configWrite("trackView.columnLayouts", _columnLayouts, uimodel::TrackColumnLayoutYamlSchema{}),
      rt::configWrite(
        "trackView.presentations", _presentationPreferences, uimodel::ListPresentationPreferenceYamlSchema{}));
  }

  Result<> LibrarySession::commitSelectedRoot()
  {
    if (_playbackPersistenceAdmission != PlaybackPersistenceAdmission::AwaitingRootCommit || !_optSelectedRootCommit)
    {
      return {};
    }

    auto candidateRes = prepareSelectedRootCommit(_settings, *_optSelectedRootCommit);

    if (!candidateRes)
    {
      _optSelectedRootCommit.reset();
      _runtimePtr->sealPlaybackSessionPersistenceWrites();
      _playbackPersistenceAdmission = PlaybackPersistenceAdmission::Sealed;
      return std::unexpected{candidateRes.error()};
    }

    auto commitRes = saveSettingsCandidate(*candidateRes);

    _optSelectedRootCommit.reset();

    if (!commitRes)
    {
      _runtimePtr->sealPlaybackSessionPersistenceWrites();
      _playbackPersistenceAdmission = PlaybackPersistenceAdmission::Sealed;
      return commitRes;
    }

    _settings = std::move(*candidateRes);
    _runtimePtr->startPlaybackSessionPersistence();
    _playbackPersistenceAdmission = PlaybackPersistenceAdmission::Ready;
    return {};
  }

  Result<> LibrarySession::retirePlaybackSessionForLibrarySwitch()
  {
    if (_playbackPersistenceAdmission == PlaybackPersistenceAdmission::Retired)
    {
      return {};
    }

    if (_shutdown || _runtimePtr == nullptr)
    {
      return makeError(Error::Code::InvalidState, "The WinUI library session is shutting down");
    }

    auto retiredRes = _runtimePtr->retirePlaybackSessionForLibrarySwitch();

    if (!retiredRes)
    {
      return retiredRes;
    }

    _playbackPersistenceAdmission = PlaybackPersistenceAdmission::Retired;
    return {};
  }

  void LibrarySession::setCallbacks(LibrarySessionCallbacks callbacks)
  {
    _callbacks = std::move(callbacks);
  }

  Result<std::unique_ptr<rt::AppRuntime>> LibrarySession::createRuntime(std::filesystem::path const& root)
  {
    auto const paths = rt::LibraryPaths{root};
    auto workspaceStorePtr = std::make_unique<rt::ConfigStore>(paths.databasePath() / "workspace.yaml");
    auto executorPtr = std::make_unique<DispatcherQueueExecutor>(_dispatcher);

    // A missing cache location is not a startup failure the way a missing state
    // root is: what lives there is derived, so the session opens without it and
    // cover reads re-extract from the media files instead.
    auto const cacheDirRes = utility::applicationCacheDirectory();
    auto runtimeRes = rt::AppRuntime::create(
      rt::AppRuntimeDependencies{.executorPtr = std::move(executorPtr),
                                 .musicRoot = root,
                                 .databasePath = paths.databasePath(),
                                 .cacheDirectory = cacheDirRes ? *cacheDirRes : std::filesystem::path{},
                                 .workspaceConfigStorePtr = std::move(workspaceStorePtr),
                                 .playbackSessionConfigStore = _playbackStorePtr.get(),
                                 .textOrderingPolicy = &_textOrderingPolicy});

    if (!runtimeRes)
    {
      return std::unexpected{runtimeRes.error()};
    }

    auto runtimePtr = std::move(*runtimeRes);

    for (auto& providerPtr : audio::createPlatformBackendProviders())
    {
      runtimePtr->addAudioProvider(std::move(providerPtr));
    }

    if (auto const restoredRes = runtimePtr->workspace().restoreSession(runtimePtr->workspaceConfigStore());
        !restoredRes)
    {
      APP_LOG_WARN("LibrarySession: failed to restore workspace for '{}': {}",
                   utility::pathToUtf8(root),
                   restoredRes.error().message);
    }

    return runtimePtr;
  }

  void LibrarySession::bindRuntimeServices()
  {
    _playbackCommandsPtr.reset();
    _presentationPreferenceLifecyclePtr.reset();
    _presentationCatalogPtr.reset();
    _presentationCatalogPtr =
      std::make_unique<uimodel::TrackPresentationCatalog>(_runtimePtr->workspace(), _textCatalog);
    _presentationPreferenceLifecyclePtr = std::make_unique<uimodel::ListPresentationPreferenceLifecycle>(
      _presentationPreferences.presentations,
      _runtimePtr->library().changes(),
      [this](ListId const)
      {
        auto const savedRes = _settingsStorePtr->save(
          "trackView.presentations", _presentationPreferences, uimodel::ListPresentationPreferenceYamlSchema{});

        if (!savedRes)
        {
          APP_LOG_WARN(
            "LibrarySession: failed to persist deleted List preference cleanup: {}", savedRes.error().message);
        }
      });
    _playbackCommandsPtr =
      std::make_unique<uimodel::PlaybackCommandSurface>(_runtimePtr->playback(), [this] { requestPlaySelection(); });
  }

  void LibrarySession::rescan() noexcept
  {
    try
    {
      if (_shutdown)
      {
        return;
      }

      if (_operationActive)
      {
        reportBusy();
        return;
      }

      _operationActive = true;
      _operationStatusKey = "winui_library_rescanning";
      reportBusy();

      startActiveScan();
    }
    catch (...)
    {
      auto exceptionPtr = std::current_exception();
      _operationStatusKey = {};
      _operationActive = false;
      AO_FATAL_EXCEPTION(std::move(exceptionPtr), "WinUI library scan start");
    }
  }

  void LibrarySession::startActiveScan()
  {
    auto const lifetimePtr = std::weak_ptr<CallbackLifetime>{_callbackLifetimePtr};
    auto present = PresentLibraryScan{[owner = this, lifetimePtr](LibraryScanResult result)
                                      {
                                        if (!lifetimePtr.expired())
                                        {
                                          owner->finishActiveScan(std::move(result));
                                        }
                                      }};
    auto* const service = &_runtimePtr->library().taskService();
    _libraryTask = _runtimePtr->async().spawnCancellable(
      [service, present = std::move(present)](std::stop_token const stopToken) mutable
      { return runActiveScan(service, std::move(present), stopToken); });
  }

  void LibrarySession::finishActiveScan(LibraryScanResult result)
  {
    if (_shutdown)
    {
      return;
    }

    _operationStatusKey = {};
    _operationActive = false;

    // What the scan amounts to, how loudly to say it, and the sentence itself
    // are decided in uimodel, so this window and the GTK one report the same
    // scan the same way. A scan that lost files says so here rather than
    // reporting a plain ready library.
    auto const outcome = uimodel::decideLibraryScanOutcome(result);
    auto const severity = uimodel::libraryScanSeverity(outcome.verdict);
    auto message = _textCatalog.libraryScanMessage(outcome);

    _runtimePtr->notifications().post(severity, message, uimodel::libraryScanLifetime(outcome.verdict));

    if (severity == rt::NotificationSeverity::Error)
    {
      reportScanFailure(outcome, std::move(message));
      return;
    }

    if (severity == rt::NotificationSeverity::Warning)
    {
      // The notification feed alone is not enough: only a shell carrying a
      // `status.activity` component presents it, and the Classic preset does
      // not. Saying "library ready" while files are missing would leave those
      // users no indication at all, so the warning takes the status line, which
      // every shipped preset shows.
      reportStatus(std::move(message));
      return;
    }

    reportReady(_runtimePtr->musicRoot());
  }

  void LibrarySession::reportScanFailure(uimodel::LibraryScanOutcome const& outcome, std::string message)
  {
    if (!_callbacks.onFailure)
    {
      return;
    }

    _callbacks.onFailure(Error{
      .code = outcome.optError ? outcome.optError->code : Error::Code::FormatRejected,
      .message = std::move(message),
    });
  }

  void LibrarySession::reportStatus(std::string status)
  {
    if (!_callbacks.onStatus)
    {
      return;
    }

    _callbacks.onStatus(std::move(status));
  }

  void LibrarySession::reportBusy()
  {
    reportStatus(resourceString(_operationStatusKey));
  }

  void LibrarySession::reportReady(std::filesystem::path const& root)
  {
    reportStatus(formatResource("winui_library_ready_at", utility::pathToUtf8(root)));
  }

  void LibrarySession::requestPlaySelection()
  {
    if (_shutdown)
    {
      return;
    }

    std::ignore = _runtimePtr->playSelectionInFocusedView();
  }

  Result<> LibrarySession::playTrack(rt::ViewId const viewId, TrackId const trackId)
  {
    if (_shutdown)
    {
      return makeError(Error::Code::InvalidState, "The WinUI library session is shutting down");
    }

    if (auto selectedRes = _runtimePtr->views().setSelection(viewId, {trackId}); !selectedRes)
    {
      return selectedRes;
    }

    if (auto focusedRes = _runtimePtr->workspace().focusView(viewId); !focusedRes)
    {
      return focusedRes;
    }

    return _runtimePtr->playback().commands().startFromView(viewId, trackId);
  }
} // namespace ao::winui
