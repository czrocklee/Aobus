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
#include <ao/audio/BackendConfig.h>
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
#include <ao/uimodel/library/presentation/ListPresentationPreferenceLifecycle.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/presentation/TrackPresentationRecommender.h>
#include <ao/uimodel/library/task/LibraryScanWorkflow.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/utility/Path.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/WinUiErrorBoundary.h>
#include <ao/winui/app/LibraryStartupPlan.h>
#include <ao/winui/app/StartupOptions.h>

#if AOBUS_HAS_WASAPI
#include <ao/audio/backend/WasapiProvider.h>
#endif

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
    using PresentLibraryScan = std::move_only_function<void(LibraryScanResult)>;

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
    StartupOptions startupOptions)
  {
    auto sessionPtr = std::unique_ptr<LibrarySession>{new LibrarySession{std::move(stateRoot), std::move(dispatcher)}};

    if (auto initializedRes = sessionPtr->initialize(std::move(startupOptions)); !initializedRes)
    {
      return std::unexpected{initializedRes.error()};
    }

    return sessionPtr;
  }

  LibrarySession::LibrarySession(std::filesystem::path stateRoot,
                                 winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher)
    : _stateRoot{std::move(stateRoot)}
    , _dispatcher{std::move(dispatcher)}
    , _settingsStorePtr{std::make_unique<rt::ConfigStore>(_stateRoot / "windows-settings.yaml")}
    , _playbackStorePtr{std::make_unique<rt::ConfigStore>(_stateRoot / "windows-playback.yaml")}
  {
  }

  Result<> LibrarySession::initialize(StartupOptions startupOptions)
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

    auto startupPlanRes = planLibraryStartup(startupOptions, _settings, _stateRoot / "empty-library");

    if (!startupPlanRes)
    {
      return makeError(startupPlanRes.error().code,
                       std::format("Failed to select the startup library: {}", startupPlanRes.error().message));
    }

    if (startupPlanRes->source == LibraryStartupRootSource::EmptyLibraryFallback)
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
    _scanAfterOpen = !rt::LibraryPaths{root}.hasExistingDatabase();
    auto runtimeRes = createRuntime(root);

    if (!runtimeRes)
    {
      return makeError(
        runtimeRes.error().code, std::format("Failed to open initial library: {}", runtimeRes.error().message));
    }

    _runtimePtr = std::move(*runtimeRes);
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

    _runtimePtr->workspace().saveSession(_runtimePtr->workspaceConfigStore());
    return _settingsStorePtr->saveTogether(
      rt::configWrite("desktop", _settings, winui::DesktopSettingsYamlSchema{}),
      rt::configWrite("trackView.columnLayouts", _columnLayouts, uimodel::TrackColumnLayoutYamlSchema{}),
      rt::configWrite(
        "trackView.presentations", _presentationPreferences, uimodel::ListPresentationPreferenceYamlSchema{}));
  }

  Result<> LibrarySession::commitSelectedRoot()
  {
    if (!_optSelectedRootCommit)
    {
      return {};
    }

    try
    {
      _optSelectedRootCommit->apply(_settings);
      _optSelectedRootCommit.reset();
    }
    catch (std::filesystem::filesystem_error const& error)
    {
      return makeError(
        Error::Code::InvalidInput, std::format("Failed to encode the selected library path: {}", error.what()));
    }

    return saveSettings();
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
    auto runtimeRes =
      rt::AppRuntime::create(rt::AppRuntimeDependencies{.executorPtr = std::move(executorPtr),
                                                        .musicRoot = root,
                                                        .databasePath = paths.databasePath(),
                                                        .workspaceConfigStorePtr = std::move(workspaceStorePtr),
                                                        .playbackSessionConfigStore = _playbackStorePtr.get()});

    if (!runtimeRes)
    {
      return std::unexpected{runtimeRes.error()};
    }

    auto runtimePtr = std::move(*runtimeRes);
#if AOBUS_HAS_WASAPI
    runtimePtr->addAudioProvider(std::make_unique<audio::backend::WasapiProvider>());
#endif

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
    _presentationCatalogPtr = std::make_unique<uimodel::TrackPresentationCatalog>(_runtimePtr->workspace());
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
      _operationStatusKey = "RescanningLibrary";
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

    if (!result)
    {
      reportFailure(result.error().error);
    }
    else if (result->disposition == uimodel::LibraryScanPlanDisposition::ErrorsOnly)
    {
      reportFailure(Error{
        .code = Error::Code::FormatRejected,
        .message = formatResource(
          result->summary.errorCount == 1 ? "LibraryScanUnreadableOneFormat" : "LibraryScanUnreadableManyFormat",
          result->summary.errorCount),
      });
    }
    else
    {
      reportReady(_runtimePtr->musicRoot());
    }
  }

  void LibrarySession::reportStatus(std::string status)
  {
    if (!_callbacks.onStatus)
    {
      return;
    }

    _callbacks.onStatus(std::move(status));
  }

  void LibrarySession::reportFailure(Error const& error)
  {
    _runtimePtr->notifications().post(rt::NotificationSeverity::Error,
                                      formatResource("ErrorFormat", error.message),
                                      rt::NotificationLifetime::history());

    if (!_callbacks.onFailure)
    {
      return;
    }

    _callbacks.onFailure(error);
  }

  void LibrarySession::reportBusy()
  {
    reportStatus(resourceString(_operationStatusKey));
  }

  void LibrarySession::reportReady(std::filesystem::path const& root)
  {
    reportStatus(formatResource("LibraryReadyFormat", utility::pathToUtf8(root)));
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
