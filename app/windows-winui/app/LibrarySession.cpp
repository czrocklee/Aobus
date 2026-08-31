// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/LibrarySession.h"

#include "app/DispatcherQueueExecutor.h"
#include "platform/StringResources.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/audio/BackendProvider.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/desktop/LibraryStartupPlanner.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/i18n/MessageCatalog.h>
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
#include <ao/rt/library/LibraryJobs.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/rt/library/LibrarySnapshot.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/uimodel/input/KeymapModel.h>
#include <ao/uimodel/input/KeymapStore.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>
#include <ao/uimodel/library/presentation/ListPresentations.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackColumnLayouts.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/task/LibraryScanOutcome.h>
#include <ao/uimodel/playback/command/PlaybackActions.h>
#include <ao/uimodel/status/activity/ActivityPresentationText.h>
#include <ao/utility/Path.h>
#include <ao/utility/PlatformDirectories.h>
#include <ao/winui/CallbackAdmissionGate.h>
#include <ao/winui/DesktopSettingsYamlSchema.h>
#include <ao/winui/WinUiErrorBoundary.h>
#include <ao/winui/app/DesktopOutputSelection.h>
#include <ao/winui/app/SelectedRootCommit.h>

#include <cstdint>
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
    using PresentLibraryScan = compat::MoveOnlyFunction<void(uimodel::LibraryScanOutcome)>;

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

    async::Task<void> runActiveScan(async::Runtime* const runtime,
                                    rt::LibraryJobs* const jobs,
                                    PresentLibraryScan present,
                                    std::stop_token const stopToken)
    {
      auto outcome = co_await uimodel::runLibraryScan(jobs, uimodel::LibraryScanMode::Eager, stopToken);
      co_await runtime->resumeOnCallbackExecutor(stopToken);
      present(std::move(outcome));
    }
  } // namespace

  enum class PlaybackPersistenceAdmission : std::uint8_t
  {
    Ready,
    AwaitingRootCommit,
    Sealed,
    Retired,
  };

  struct InteractiveBorrowers final
  {
    InteractiveBorrowers(rt::AppRuntime& runtime,
                         i18n::MessageCatalog const& textCatalog,
                         uimodel::ListPresentations::Snapshot restoredListPresentations,
                         std::function<void(ListId)> persistListPresentations,
                         std::function<void()> requestPlaySelection)
      : presentationCatalog{runtime.workspace(), textCatalog}
      , listPresentations{presentationCatalog, runtime.library().changes()}
      , playbackActions{runtime.playback(), std::move(requestPlaySelection)}
    {
      listPresentations.restore(std::move(restoredListPresentations));
      listPresentationsSub = listPresentations.signalChanged().connect(std::move(persistListPresentations));
    }

    uimodel::TrackPresentationCatalog presentationCatalog;
    uimodel::ListPresentations listPresentations;
    async::Subscription listPresentationsSub;
    uimodel::PlaybackActions playbackActions;
  };

  struct RuntimeGraph final
  {
    RuntimeGraph(rt::AppRuntime&& runtimeValue, DispatcherQueueExecutor& dispatcherExecutorValue)
      : runtime{std::move(runtimeValue)}, dispatcherExecutor{dispatcherExecutorValue}
    {
    }

    void shutdown() noexcept
    {
      if (stopped)
      {
        return;
      }

      optInteractiveBorrowers.reset();
      runtime.shutdown();
      dispatcherExecutor.completeClosing();
      stopped = true;
    }

    rt::AppRuntime runtime;
    DispatcherQueueExecutor& dispatcherExecutor;
    std::optional<InteractiveBorrowers> optInteractiveBorrowers;
    bool stopped = false;
  };

  struct LibrarySession::Storage final
  {
    Storage(std::filesystem::path stateRootValue,
            winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcherValue,
            i18n::MessageCatalog textCatalogValue,
            rt::TextOrderingPolicy const& textOrderingPolicyValue,
            rt::CompletionAliasPolicy const& completionAliasPolicyValue)
      : stateRoot{std::move(stateRootValue)}
      , dispatcher{std::move(dispatcherValue)}
      , textCatalog{std::move(textCatalogValue)}
      , textOrderingPolicy{textOrderingPolicyValue}
      , completionAliasPolicy{completionAliasPolicyValue}
      , settingsStorePtr{std::make_unique<rt::ConfigStore>(stateRoot / "windows-settings.yaml")}
      , playbackStorePtr{std::make_unique<rt::ConfigStore>(stateRoot / "windows-playback.yaml")}
    {
    }

    std::filesystem::path stateRoot;
    winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher{nullptr};
    i18n::MessageCatalog textCatalog;
    rt::TextOrderingPolicy const& textOrderingPolicy;
    rt::CompletionAliasPolicy const& completionAliasPolicy;
    std::unique_ptr<rt::ConfigStore> settingsStorePtr;
    std::unique_ptr<rt::ConfigStore> playbackStorePtr;
    DesktopSettings settings{};
    uimodel::TrackColumnLayouts columnLayouts{};
    uimodel::ListPresentations::Snapshot restoredListPresentations{};
    uimodel::KeymapModel keymap{};
    std::optional<std::filesystem::path> optSelectedRootCommit;
    std::optional<RuntimeGraph> optRuntimeGraph;
    LibrarySessionCallbacks callbacks{};
    async::TaskHandle libraryTask;
    CallbackAdmissionGate ownerCallbackGate;
    std::string_view operationStatusKey;
    bool operationActive = false;
    bool scanAfterOpen = false;
    bool shutdown = false;
    PlaybackPersistenceAdmission playbackPersistenceAdmission = PlaybackPersistenceAdmission::Ready;
  };

  Result<std::unique_ptr<LibrarySession>> LibrarySession::create(
    std::filesystem::path stateRoot,
    winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
    i18n::MessageCatalog textCatalog,
    rt::TextOrderingPolicy const& textOrderingPolicy,
    rt::CompletionAliasPolicy const& completionAliasPolicy,
    std::optional<desktop::LibrarySwitchRequest> optSuccessorRequest)
  {
    auto sessionPtr = std::unique_ptr<LibrarySession>{new LibrarySession{
      std::move(stateRoot), std::move(dispatcher), std::move(textCatalog), textOrderingPolicy, completionAliasPolicy}};

    if (auto initializedRes = sessionPtr->initialize(std::move(optSuccessorRequest)); !initializedRes)
    {
      return std::unexpected{initializedRes.error()};
    }

    return sessionPtr;
  }

  LibrarySession::LibrarySession(std::filesystem::path stateRoot,
                                 winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher,
                                 i18n::MessageCatalog textCatalog,
                                 rt::TextOrderingPolicy const& textOrderingPolicy,
                                 rt::CompletionAliasPolicy const& completionAliasPolicy)
    : _storagePtr{std::make_unique<Storage>(std::move(stateRoot),
                                            std::move(dispatcher),
                                            std::move(textCatalog),
                                            textOrderingPolicy,
                                            completionAliasPolicy)}
  {
  }

  Result<> LibrarySession::initialize(std::optional<desktop::LibrarySwitchRequest> optSuccessorRequest)
  {
    auto& storage = *_storagePtr;
    auto directoryEc = std::error_code{};
    std::filesystem::create_directories(storage.stateRoot, directoryEc);

    if (directoryEc)
    {
      return makeError(
        Error::Code::IoError, std::format("Failed to create the WinUI state directory: {}", directoryEc.message()));
    }

    if (auto loadedRes =
          storage.settingsStorePtr->load("desktop", storage.settings, winui::DesktopSettingsYamlSchema{});
        !loadedRes && loadedRes.error().code != Error::Code::NotFound)
    {
      APP_LOG_WARN("LibrarySession: failed to load Windows settings: {}", loadedRes.error().message);
    }

    auto columnLayouts = uimodel::TrackColumnLayouts::Snapshot{};

    if (auto loadedRes = storage.settingsStorePtr->load(
          uimodel::kTrackColumnLayoutsConfigGroup, columnLayouts, uimodel::TrackColumnLayoutYamlSchema{});
        loadedRes)
    {
      storage.columnLayouts.restore(std::move(columnLayouts));
    }
    else if (loadedRes.error().code != Error::Code::NotFound)
    {
      APP_LOG_WARN("LibrarySession: failed to load Windows column layouts: {}", loadedRes.error().message);
    }

    if (auto loadedRes = storage.settingsStorePtr->load(uimodel::kListPresentationsConfigGroup,
                                                        storage.restoredListPresentations,
                                                        uimodel::ListPresentationPreferenceYamlSchema{});
        !loadedRes && loadedRes.error().code != Error::Code::NotFound)
    {
      APP_LOG_WARN("LibrarySession: failed to load Windows presentation preferences: {}", loadedRes.error().message);
    }

    // Shortcuts share the settings file: one store, one process, so the whole
    // document is still written as a unit.
    storage.keymap = uimodel::loadKeymap(*storage.settingsStorePtr, uimodel::defaultKeymap());

    auto optPersistedRoot = std::optional<std::filesystem::path>{};

    if (!storage.settings.lastLibraryPath.empty())
    {
      try
      {
        optPersistedRoot = utility::pathFromUtf8(storage.settings.lastLibraryPath);
      }
      catch (std::filesystem::filesystem_error const&)
      {
        optPersistedRoot.reset();
      }
    }

    auto startupPlanRes = desktop::planLibraryStartup({
      .optSuccessorRequest = std::move(optSuccessorRequest),
      .optPersistedRoot = std::move(optPersistedRoot),
      .emptyLibraryRoot = storage.stateRoot / "empty-library",
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
    storage.optSelectedRootCommit = std::move(startupPlanRes->optSelectedRootCommit);
    storage.playbackPersistenceAdmission =
      startupPlanRes->playbackPersistence == desktop::PlaybackPersistenceStartup::Restore
        ? PlaybackPersistenceAdmission::Ready
        : PlaybackPersistenceAdmission::AwaitingRootCommit;
    storage.scanAfterOpen = startupPlanRes->scanAfterOpen || !rt::LibraryPaths{root}.hasExistingDatabase();

    if (auto runtimeRes = emplaceRuntimeGraph(root); !runtimeRes)
    {
      return makeError(
        runtimeRes.error().code, std::format("Failed to open initial library: {}", runtimeRes.error().message));
    }

    auto& runtime = storage.optRuntimeGraph->runtime;

    for (auto& providerPtr : audio::createPlatformBackendProviders())
    {
      runtime.addAudioProvider(std::move(providerPtr));
    }

    if (auto const restoredRes = runtime.workspace().restoreSession(runtime.workspaceConfigStore()); !restoredRes)
    {
      APP_LOG_WARN("LibrarySession: failed to restore workspace for '{}': {}",
                   utility::pathToUtf8(root),
                   restoredRes.error().message);
    }

    auto& playback = runtime.playback();
    auto const optOutputSelection =
      resolveDesktopOutputSelectionToRestore(storage.settings, playback.snapshot().transport.output);

    if (optOutputSelection)
    {
      playback.commands().setOutputDevice(
        optOutputSelection->backendId, optOutputSelection->deviceId, optOutputSelection->profileId);
    }

    if (storage.playbackPersistenceAdmission == PlaybackPersistenceAdmission::Ready)
    {
      runtime.startPlaybackSessionPersistence();

      if (auto restoredRes = runtime.restorePlaybackSession(); !restoredRes)
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
    auto& storage = *_storagePtr;

    if (storage.shutdown)
    {
      return;
    }

    storage.shutdown = true;

    if (storage.optRuntimeGraph)
    {
      // Classify native wake rejection as expected while final runtime
      // publications are still admitted and before cancellation can wake it.
      storage.optRuntimeGraph->dispatcherExecutor.beginClosing();
    }

    // Admission tokens never protect this object's memory. Retire them before
    // cancellation, then join every producer before releasing the facade.
    storage.ownerCallbackGate.retire();
    storage.callbacks = {};
    storage.operationActive = false;
    storage.operationStatusKey = {};
    storage.libraryTask.reset();

    if (storage.optRuntimeGraph)
    {
      checkpointWorkspaceBestEffort(storage.optRuntimeGraph->runtime);
      storage.optRuntimeGraph->shutdown();
    }

    // RuntimeGraph destroys interactive borrowers before the runtime, whose
    // own graph retires before the settings stores declared earlier in Storage.
    storage.optRuntimeGraph.reset();
  }

  rt::AppRuntime& LibrarySession::runtime() const noexcept
  {
    return _storagePtr->optRuntimeGraph->runtime;
  }

  std::filesystem::path const& LibrarySession::musicRoot() const noexcept
  {
    return runtime().musicRoot();
  }

  bool LibrarySession::scanAfterOpen() const noexcept
  {
    return _storagePtr->scanAfterOpen;
  }

  bool LibrarySession::operationActive() const noexcept
  {
    return _storagePtr->operationActive;
  }

  uimodel::PlaybackActions& LibrarySession::playbackActions() const noexcept
  {
    return _storagePtr->optRuntimeGraph->optInteractiveBorrowers->playbackActions;
  }

  i18n::MessageCatalog const& LibrarySession::textCatalog() const noexcept
  {
    return _storagePtr->textCatalog;
  }

  DesktopSettings const& LibrarySession::settings() const noexcept
  {
    return _storagePtr->settings;
  }

  DesktopSettings& LibrarySession::settings() noexcept
  {
    return _storagePtr->settings;
  }

  uimodel::TrackColumnLayouts const& LibrarySession::columnLayouts() const noexcept
  {
    return _storagePtr->columnLayouts;
  }

  uimodel::TrackColumnLayouts& LibrarySession::columnLayouts() noexcept
  {
    return _storagePtr->columnLayouts;
  }

  uimodel::TrackPresentationCatalog& LibrarySession::presentationCatalog() const noexcept
  {
    return _storagePtr->optRuntimeGraph->optInteractiveBorrowers->presentationCatalog;
  }

  uimodel::ListPresentations& LibrarySession::listPresentations() const noexcept
  {
    return _storagePtr->optRuntimeGraph->optInteractiveBorrowers->listPresentations;
  }

  uimodel::KeymapModel const& LibrarySession::keymap() const noexcept
  {
    return _storagePtr->keymap;
  }

  std::filesystem::path const& LibrarySession::stateRoot() const noexcept
  {
    return _storagePtr->stateRoot;
  }

  rt::TrackPresentationSpec LibrarySession::presentationForList(ListId const listId) const
  {
    auto context = uimodel::ListPresentationContext{
      .listId = listId,
      .sourceKind = uimodel::ListPresentationSourceKind::AllTracks,
    };
    auto& graph = *_storagePtr->optRuntimeGraph;
    auto& listPresentations = graph.optInteractiveBorrowers->listPresentations;

    if (!rt::isVirtualListId(listId))
    {
      if (auto const optNode = graph.runtime.library().snapshot().listNode(listId); optNode)
      {
        context.sourceKind = uimodel::ListPresentationSourceKind::SavedList;
        context.listExpression = optNode->expression;
        return listPresentations.presentationForList(context);
      }
    }

    return listPresentations.presentationForList(context);
  }

  Result<> LibrarySession::saveSettings()
  {
    if (_storagePtr->shutdown)
    {
      return makeError(Error::Code::InvalidState, "The WinUI library session is shutting down");
    }

    return saveSettingsCandidate(_storagePtr->settings);
  }

  void LibrarySession::setPreferredOutputSelection(audio::OutputDeviceSelection const& selection) noexcept
  {
    std::ignore = rememberDesktopOutputSelection(_storagePtr->settings, selection);
  }

  Result<> LibrarySession::saveSettingsCandidate(DesktopSettings const& settings)
  {
    auto& storage = *_storagePtr;
    AO_INVARIANT(storage.optRuntimeGraph && storage.optRuntimeGraph->optInteractiveBorrowers,
                 "LibrarySession cannot save settings before List presentations are bound");
    auto& graph = *storage.optRuntimeGraph;
    graph.runtime.workspace().saveSession(graph.runtime.workspaceConfigStore());
    auto const columnLayouts = storage.columnLayouts.snapshot();
    auto const listPresentations = graph.optInteractiveBorrowers->listPresentations.snapshot();
    return storage.settingsStorePtr->saveTogether(
      rt::configWrite("desktop", settings, winui::DesktopSettingsYamlSchema{}),
      rt::configWrite(uimodel::kTrackColumnLayoutsConfigGroup, columnLayouts, uimodel::TrackColumnLayoutYamlSchema{}),
      rt::configWrite(
        uimodel::kListPresentationsConfigGroup, listPresentations, uimodel::ListPresentationPreferenceYamlSchema{}));
  }

  Result<> LibrarySession::commitSelectedRoot()
  {
    auto& storage = *_storagePtr;

    if (storage.playbackPersistenceAdmission != PlaybackPersistenceAdmission::AwaitingRootCommit ||
        !storage.optSelectedRootCommit)
    {
      return {};
    }

    auto candidateRes = prepareSelectedRootCommit(storage.settings, *storage.optSelectedRootCommit);
    auto& runtime = storage.optRuntimeGraph->runtime;

    if (!candidateRes)
    {
      storage.optSelectedRootCommit.reset();
      runtime.sealPlaybackSessionPersistenceWrites();
      storage.playbackPersistenceAdmission = PlaybackPersistenceAdmission::Sealed;
      return std::unexpected{candidateRes.error()};
    }

    auto commitRes = saveSettingsCandidate(*candidateRes);
    storage.optSelectedRootCommit.reset();

    if (!commitRes)
    {
      runtime.sealPlaybackSessionPersistenceWrites();
      storage.playbackPersistenceAdmission = PlaybackPersistenceAdmission::Sealed;
      return commitRes;
    }

    storage.settings = std::move(*candidateRes);
    runtime.startPlaybackSessionPersistence();
    storage.playbackPersistenceAdmission = PlaybackPersistenceAdmission::Ready;
    return {};
  }

  Result<> LibrarySession::retirePlaybackSessionForLibrarySwitch()
  {
    auto& storage = *_storagePtr;

    if (storage.playbackPersistenceAdmission == PlaybackPersistenceAdmission::Retired)
    {
      return {};
    }

    if (storage.shutdown || !storage.optRuntimeGraph)
    {
      return makeError(Error::Code::InvalidState, "The WinUI library session is shutting down");
    }

    auto retiredRes = storage.optRuntimeGraph->runtime.retirePlaybackSessionForLibrarySwitch();

    if (!retiredRes)
    {
      return retiredRes;
    }

    storage.playbackPersistenceAdmission = PlaybackPersistenceAdmission::Retired;
    return {};
  }

  void LibrarySession::setCallbacks(LibrarySessionCallbacks callbacks)
  {
    _storagePtr->callbacks = std::move(callbacks);
  }

  Result<> LibrarySession::emplaceRuntimeGraph(std::filesystem::path const& root)
  {
    auto& storage = *_storagePtr;
    AO_INVARIANT(!storage.optRuntimeGraph, "LibrarySession cannot replace a live runtime graph");
    auto const paths = rt::LibraryPaths{root};
    auto workspaceStorePtr = std::make_unique<rt::ConfigStore>(paths.databasePath() / "workspace.yaml");
    auto executorPtr = std::make_unique<DispatcherQueueExecutor>(storage.dispatcher);
    auto& dispatcherExecutor = *executorPtr;

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
                                 .playbackSessionConfigStore = storage.playbackStorePtr.get(),
                                 .textOrderingPolicy = &storage.textOrderingPolicy,
                                 .completionAliasPolicy = &storage.completionAliasPolicy});

    if (!runtimeRes)
    {
      return std::unexpected{runtimeRes.error()};
    }

    // This is the sole post-factory move. Nothing may publish a callback or a
    // provider borrow until the runtime has reached this final storage address.
    storage.optRuntimeGraph.emplace(std::move(*runtimeRes), dispatcherExecutor);
    return {};
  }

  void LibrarySession::bindRuntimeServices()
  {
    auto& storage = *_storagePtr;
    auto& graph = *storage.optRuntimeGraph;
    AO_INVARIANT(!graph.optInteractiveBorrowers, "LibrarySession cannot bind interactive runtime borrowers twice");
    graph.optInteractiveBorrowers.emplace(
      graph.runtime,
      storage.textCatalog,
      std::move(storage.restoredListPresentations),
      [this](ListId const)
      {
        auto& callbackStorage = *_storagePtr;
        auto const listPresentations =
          callbackStorage.optRuntimeGraph->optInteractiveBorrowers->listPresentations.snapshot();
        auto const savedRes = callbackStorage.settingsStorePtr->save(
          uimodel::kListPresentationsConfigGroup, listPresentations, uimodel::ListPresentationPreferenceYamlSchema{});

        if (!savedRes)
        {
          APP_LOG_WARN("LibrarySession: failed to persist presentation preference: {}", savedRes.error().message);
        }
      },
      [this] { requestPlaySelection(); });
    storage.restoredListPresentations.clear();
  }

  void LibrarySession::rescan() noexcept
  {
    auto& storage = *_storagePtr;

    try
    {
      if (storage.shutdown)
      {
        return;
      }

      if (storage.operationActive)
      {
        reportBusy();
        return;
      }

      storage.operationActive = true;
      storage.operationStatusKey = "winui_library_rescanning";
      reportBusy();

      startActiveScan();
    }
    catch (...)
    {
      auto exceptionPtr = std::current_exception();
      storage.operationStatusKey = {};
      storage.operationActive = false;
      AO_FATAL_EXCEPTION(std::move(exceptionPtr), "WinUI library scan start");
    }
  }

  void LibrarySession::startActiveScan()
  {
    auto& storage = *_storagePtr;
    auto const token = storage.ownerCallbackGate.token();
    auto present = PresentLibraryScan{[owner = this, token](uimodel::LibraryScanOutcome outcome)
                                      {
                                        if (token.admits())
                                        {
                                          owner->finishActiveScan(std::move(outcome));
                                        }
                                      }};
    auto& appRuntime = storage.optRuntimeGraph->runtime;
    auto* const runtime = &appRuntime.async();
    auto* const jobs = &appRuntime.library().jobs();
    storage.libraryTask = appRuntime.async().spawnCancellable(
      [runtime, jobs, present = std::move(present)](std::stop_token const stopToken) mutable
      { return runActiveScan(runtime, jobs, std::move(present), stopToken); });
  }

  void LibrarySession::finishActiveScan(uimodel::LibraryScanOutcome outcome)
  {
    auto& storage = *_storagePtr;

    if (storage.shutdown)
    {
      return;
    }

    storage.operationStatusKey = {};
    storage.operationActive = false;

    // What the scan amounts to, how loudly to say it, and the sentence itself
    // are decided in uimodel, so this window and the GTK one report the same
    // scan the same way. A scan that lost files says so here rather than
    // reporting a plain ready library.
    auto const severity = uimodel::libraryScanSeverity(outcome.verdict);
    auto message = formatLibraryScanMessage(storage.textCatalog, outcome);
    auto& appRuntime = storage.optRuntimeGraph->runtime;

    appRuntime.notifications().post(severity, message, uimodel::libraryScanLifetime(outcome.verdict));

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

    reportReady(appRuntime.musicRoot());
  }

  void LibrarySession::reportScanFailure(uimodel::LibraryScanOutcome const& outcome, std::string message)
  {
    auto& callback = _storagePtr->callbacks.onFailure;

    if (!callback)
    {
      return;
    }

    callback(Error{
      .code = outcome.optError ? outcome.optError->code : Error::Code::FormatRejected,
      .message = std::move(message),
    });
  }

  void LibrarySession::reportStatus(std::string status)
  {
    auto& callback = _storagePtr->callbacks.onStatus;

    if (!callback)
    {
      return;
    }

    callback(std::move(status));
  }

  void LibrarySession::reportBusy()
  {
    reportStatus(resourceString(_storagePtr->operationStatusKey));
  }

  void LibrarySession::reportReady(std::filesystem::path const& root)
  {
    reportStatus(formatResource("winui_library_ready_at", utility::pathToUtf8(root)));
  }

  void LibrarySession::requestPlaySelection()
  {
    auto& storage = *_storagePtr;

    if (storage.shutdown)
    {
      return;
    }

    std::ignore = storage.optRuntimeGraph->runtime.playSelectionInFocusedView();
  }

  Result<> LibrarySession::playTrack(rt::ViewId const viewId, TrackId const trackId)
  {
    auto& storage = *_storagePtr;

    if (storage.shutdown)
    {
      return makeError(Error::Code::InvalidState, "The WinUI library session is shutting down");
    }

    auto& appRuntime = storage.optRuntimeGraph->runtime;

    if (auto selectedRes = appRuntime.views().setSelection(viewId, {trackId}); !selectedRes)
    {
      return selectedRes;
    }

    if (auto focusedRes = appRuntime.workspace().focusView(viewId); !focusedRes)
    {
      return focusedRes;
    }

    return appRuntime.playback().commands().startFromView(viewId, trackId);
  }
} // namespace ao::winui
