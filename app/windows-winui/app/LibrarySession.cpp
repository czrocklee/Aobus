// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/LibrarySession.h"

#include "app/DispatcherQueueExecutor.h"
#include "platform/WindowsStringResources.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/ExceptionFormat.h>
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
#include <ao/uimodel/layout/shell/WindowsDesktopSettingsYamlSchema.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceLifecycle.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceStore.h>
#include <ao/uimodel/library/presentation/ListPresentationPreferenceYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackColumnLayoutYamlSchema.h>
#include <ao/uimodel/library/presentation/TrackPresentationCatalog.h>
#include <ao/uimodel/library/presentation/TrackPresentationRecommender.h>
#include <ao/uimodel/library/task/LibraryScanWorkflow.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/utility/Path.h>

#if AOBUS_HAS_WASAPI
#include <ao/audio/backend/WasapiProvider.h>
#endif

#include <exception>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace ao::winui
{
  namespace
  {
    using LibraryScanResult = std::expected<uimodel::LibraryScanWorkflowResult, uimodel::LibraryScanWorkflowFailure>;
    using PresentLibraryScan = std::move_only_function<void(LibraryScanResult) noexcept>;

    void quarantineRuntime(std::shared_ptr<rt::AppRuntime> runtimePtr) noexcept
    {
      if (runtimePtr == nullptr)
      {
        return;
      }

      // Dispatcher shutdown can reject the retirement hop while callbacks are
      // unwinding. Leak the last reference instead of destroying an
      // owner-affine runtime on that stack.
      // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,bugprone-unhandled-exception-at-new)
      [[maybe_unused]] auto* const leakedRuntime = new std::shared_ptr<rt::AppRuntime>{std::move(runtimePtr)};
    }

    bool sameDirectory(std::filesystem::path const& left, std::filesystem::path const& right) noexcept
    {
      auto error = std::error_code{};
      auto const equivalent = std::filesystem::equivalent(left, right, error);
      return !error ? equivalent : left.lexically_normal() == right.lexically_normal();
    }

    struct RuntimeRetirement final
    {
      std::shared_ptr<rt::AppRuntime> runtimePtr;
    };

    void deferRuntimeRelease(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& dispatcher,
                             std::shared_ptr<rt::AppRuntime> runtimePtr) noexcept
    {
      if (runtimePtr == nullptr)
      {
        return;
      }

      auto retirementPtr = std::shared_ptr<RuntimeRetirement>{};

      try
      {
        retirementPtr = std::make_shared<RuntimeRetirement>();
      }
      catch (...)
      {
        quarantineRuntime(std::move(runtimePtr));
        return;
      }

      retirementPtr->runtimePtr = std::move(runtimePtr);
      bool queued = false;

      try
      {
        queued = dispatcher.TryEnqueue([retirementPtr] { retirementPtr->runtimePtr.reset(); });
      }
      catch (...)
      {
        queued = false;
      }

      if (!queued)
      {
        APP_LOG_CRITICAL("LibrarySession: UI dispatcher rejected old runtime retirement");
        quarantineRuntime(std::move(retirementPtr->runtimePtr));
      }
    }

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

    async::Task<void> runActiveScan(rt::LibraryTaskService* const service,
                                    PresentLibraryScan present,
                                    std::stop_token const stopToken)
    {
      auto result = co_await uimodel::runLibraryScanWorkflow(service, uimodel::LibraryScanMode::Eager, stopToken);
      async::throwIfStopRequested(stopToken);
      present(std::move(result));
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

    auto runtimeResult = createRuntime(root);

    if (!runtimeResult)
    {
      throwException<Exception>("Failed to open initial library: {}", runtimeResult.error().message);
    }

    _runtimePtr = std::move(*runtimeResult);
    bindRuntimeServices();
  }

  LibrarySession::~LibrarySession()
  {
    _callbackLifetimePtr.reset();
    _libraryTask.reset();
    _callbacks = {};

    if (_runtimePtr != nullptr)
    {
      checkpointWorkspace(*_runtimePtr, "session teardown");
    }

    _playbackCommandsPtr.reset();
    _presentationPreferenceLifecyclePtr.reset();
    _presentationCatalogPtr.reset();
    _runtimePtr.reset();
  }

  rt::AppRuntime& LibrarySession::runtime() const noexcept
  {
    return *_runtimePtr;
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
    checkpointWorkspace(*_runtimePtr, "settings save");
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

  Result<std::shared_ptr<rt::AppRuntime>> LibrarySession::createRuntime(std::filesystem::path const& root)
  {
    auto const paths = rt::LibraryPaths{root};
    auto workspaceStorePtr = std::make_unique<rt::ConfigStore>(paths.databasePath() / "workspace.yaml");
    auto executorPtr = std::make_unique<DispatcherQueueExecutor>(_dispatcher);
    auto runtimeResult =
      rt::AppRuntime::create(rt::AppRuntimeDependencies{.executorPtr = std::move(executorPtr),
                                                        .musicRoot = root,
                                                        .databasePath = paths.databasePath(),
                                                        .workspaceConfigStorePtr = std::move(workspaceStorePtr),
                                                        .playbackSessionConfigStore = _playbackStorePtr.get(),
                                                        .asyncExceptionHandler = rt::Log::asyncExceptionHandler()});

    if (!runtimeResult)
    {
      return std::unexpected{runtimeResult.error()};
    }

    auto runtimePtr = std::shared_ptr<rt::AppRuntime>{std::move(*runtimeResult)};
#if AOBUS_HAS_WASAPI
    runtimePtr->addAudioProvider(std::make_unique<audio::backend::WasapiProvider>());
#endif

    if (auto const restored = runtimePtr->workspace().restoreSession(runtimePtr->workspaceConfigStore()); !restored)
    {
      APP_LOG_WARN("LibrarySession: failed to restore workspace for '{}': {}",
                   utility::pathToUtf8(root),
                   restored.error().message);
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
    _playbackCommandsPtr =
      std::make_unique<uimodel::PlaybackCommandSurface>(_runtimePtr->playback(), [this] { requestPlaySelection(); });
  }

  void LibrarySession::openLibrary(std::filesystem::path root)
  {
    root = std::filesystem::absolute(root).lexically_normal();

    if (_operationActive)
    {
      reportBusy();
      return;
    }

    if (sameDirectory(root, _runtimePtr->musicRoot()))
    {
      reportReady(root);
      return;
    }

    auto const scanAfterInstall = !rt::LibraryPaths{root}.hasExistingDatabase();
    _operationActive = true;
    _operationStatusKey = "PreparingLibrary";
    reportBusy();
    auto next = createRuntime(root);

    if (!next)
    {
      _operationActive = false;
      _operationStatusKey = {};
      reportFailure(next.error());
      return;
    }

    installRuntime(std::move(*next), root, scanAfterInstall);
  }

  void LibrarySession::rescan()
  {
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

  void LibrarySession::installRuntime(std::shared_ptr<rt::AppRuntime> nextPtr,
                                      std::filesystem::path const& root,
                                      bool const scanAfterInstall) noexcept
  {
    checkpointWorkspace(*_runtimePtr, "library replacement");

    if (!_callbacks.onRuntimeChanging || !_callbacks.onRuntimeChanged)
    {
      std::terminate();
    }

    _callbacks.onRuntimeChanging();
    auto oldPtr = std::exchange(_runtimePtr, std::move(nextPtr));
    bindRuntimeServices();
    _callbacks.onRuntimeChanged();
    _settings.lastLibraryPath = utility::pathToUtf8(root);

    try
    {
      if (auto const saved = saveSettings(); !saved)
      {
        APP_LOG_WARN("LibrarySession: failed to persist selected library: {}", saved.error().message);
      }
    }
    catch (std::exception const& error)
    {
      APP_LOG_WARN("LibrarySession: failed to persist selected library: {}", error.what());
    }
    catch (...)
    {
      APP_LOG_WARN("LibrarySession: failed to persist selected library");
    }

    deferRuntimeRelease(_dispatcher, std::move(oldPtr));

    if (scanAfterInstall)
    {
      _operationStatusKey = "RescanningLibrary";
      reportBusy();
      startActiveScan();
      return;
    }

    reportReady(root);
    _operationStatusKey = {};
    _operationActive = false;
  }

  void LibrarySession::startActiveScan()
  {
    auto const lifetimePtr = std::weak_ptr<CallbackLifetime>{_callbackLifetimePtr};
    auto present = PresentLibraryScan{[owner = this, lifetimePtr](LibraryScanResult result) noexcept
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

  void LibrarySession::finishActiveScan(LibraryScanResult result) noexcept
  {
    try
    {
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
    catch (std::exception const& error)
    {
      APP_LOG_WARN("LibrarySession: scan presentation failed: {}", error.what());
    }
    catch (...)
    {
      APP_LOG_WARN("LibrarySession: scan presentation failed");
    }

    _operationStatusKey = {};
    _operationActive = false;
  }

  void LibrarySession::reportStatus(std::string status) noexcept
  {
    if (!_callbacks.onStatus)
    {
      return;
    }

    try
    {
      _callbacks.onStatus(std::move(status));
    }
    catch (std::exception const& error)
    {
      APP_LOG_WARN("LibrarySession: status callback failed: {}", error.what());
    }
    catch (...)
    {
      APP_LOG_WARN("LibrarySession: status callback failed");
    }
  }

  void LibrarySession::reportFailure(Error const& error) noexcept
  {
    try
    {
      _runtimePtr->notifications().post(rt::NotificationSeverity::Error,
                                        formatResource("ErrorFormat", error.message),
                                        rt::NotificationLifetime::history());
    }
    catch (std::exception const& notificationError)
    {
      APP_LOG_WARN("LibrarySession: failure notification failed: {}", notificationError.what());
    }
    catch (...)
    {
      APP_LOG_WARN("LibrarySession: failure notification failed");
    }

    if (!_callbacks.onFailure)
    {
      return;
    }

    try
    {
      _callbacks.onFailure(error);
    }
    catch (std::exception const& callbackError)
    {
      APP_LOG_WARN("LibrarySession: failure callback failed: {}", callbackError.what());
    }
    catch (...)
    {
      APP_LOG_WARN("LibrarySession: failure callback failed");
    }
  }

  void LibrarySession::reportBusy() noexcept
  {
    try
    {
      reportStatus(resourceString(_operationStatusKey));
    }
    catch (std::exception const& error)
    {
      APP_LOG_WARN("LibrarySession: busy status formatting failed: {}", error.what());
    }
    catch (...)
    {
      APP_LOG_WARN("LibrarySession: busy status formatting failed");
    }
  }

  void LibrarySession::reportReady(std::filesystem::path const& root) noexcept
  {
    try
    {
      reportStatus(formatResource("LibraryReadyFormat", utility::pathToUtf8(root)));
    }
    catch (std::exception const& error)
    {
      APP_LOG_WARN("LibrarySession: ready status formatting failed: {}", error.what());
    }
    catch (...)
    {
      APP_LOG_WARN("LibrarySession: ready status formatting failed");
    }
  }

  void LibrarySession::requestPlaySelection()
  {
    std::ignore = _runtimePtr->playSelectionInFocusedView();
  }

  Result<> LibrarySession::playTrack(rt::ViewId const viewId, TrackId const trackId)
  {
    if (auto selected = _runtimePtr->views().setSelection(viewId, {trackId}); !selected)
    {
      return selected;
    }

    if (auto focused = _runtimePtr->workspace().focusView(viewId); !focused)
    {
      return focused;
    }

    return _runtimePtr->playback().commands().startFromView(viewId, trackId);
  }
} // namespace ao::winui
