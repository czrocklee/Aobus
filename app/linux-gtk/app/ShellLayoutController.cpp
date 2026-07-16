// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellLayoutController.h"

#include "AppConfigStore.h"
#include "ShellLayoutComponentStateStore.h"
#include "ShellLayoutStore.h"
#include "app/GtkUiDependencies.h"
#include "app/ThemeCoordinator.h"
#include "layout/document/GtkLayoutPresets.h"
#include "layout/document/LayoutDocument.h"
#include "layout/editor/LayoutEditorDialog.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/GioActionBridge.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutHost.h"
#include "layout/runtime/LayoutRuntime.h"
#include "playback/AobusSoulWindow.h"
#include "playback/OutputDevicePopover.h"
#include "tag/TagEditController.h"
#include <ao/CoreIds.h>
#include <ao/Exception.h>
#include <ao/async/OperationCancelled.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/rt/AppPrefsState.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/Log.h>
#include <ao/rt/PlaybackSequenceService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/layout/action/LayoutActionActivation.h>
#include <ao/uimodel/layout/action/LayoutActionAvailability.h>
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutStatePromoter.h>
#include <ao/uimodel/layout/document/LayoutNodeId.h>
#include <ao/uimodel/layout/shell/ShellLayoutSessionModel.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>

#include <gtkmm/dialog.h>
#include <gtkmm/object.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/window.h>

#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    struct LayoutLoadResult final
    {
      std::string presetId;
      uimodel::LayoutDocument document;
      uimodel::LayoutComponentStateDocument componentState;
    };

    LayoutLoadResult loadLayoutOnWorker(ShellLayoutStore& store,
                                        ShellLayoutComponentStateStore* componentStateStore,
                                        AppConfigStore& configStore)
    {
      auto prefs = rt::AppPrefsState{};
      configStore.loadAppPrefs(prefs);

      static constexpr auto kSupportedPresets = std::array<std::string_view, 2>{"classic", "modern"};

      auto const selection = uimodel::ShellLayoutSessionModel::selectPreset(prefs.lastLayoutPreset, kSupportedPresets);

      if (selection.usedFallback)
      {
        APP_LOG_DEBUG(
          "ShellLayoutController: Unknown layout preset '{}', falling back to classic", prefs.lastLayoutPreset);
      }

      auto const presetId = layout::presetIdFromString(selection.presetId);
      auto optDoc = store.load(selection.presetId);
      auto doc = optDoc ? std::move(*optDoc) : layout::makeBuiltInLayout(presetId);
      auto stateDoc = componentStateStore == nullptr
                        ? uimodel::ShellLayoutSessionModel::emptyComponentState(selection.presetId)
                        : componentStateStore->load(selection.presetId)
                            .value_or(uimodel::ShellLayoutSessionModel::emptyComponentState(selection.presetId));

      return {.presetId = selection.presetId, .document = std::move(doc), .componentState = std::move(stateDoc)};
    }

    uimodel::PlaybackCommandSurface& commandSurface(uimodel::PlaybackCommandSurface* surface)
    {
      if (surface == nullptr)
      {
        throwException<Exception>("ShellLayoutController: playback command surface is not bound");
      }

      return *surface;
    }

    ThemeCoordinator& requireThemeCoordinator(GtkUiDependencies const& dependencies)
    {
      if (dependencies.themeCoordinator == nullptr)
      {
        throwException<Exception>("ShellLayoutController: theme coordinator is not bound");
      }

      return *dependencies.themeCoordinator;
    }
  } // namespace
  ShellLayoutController::ShellLayoutController(rt::AppRuntime& runtime,
                                               Gtk::Window& window,
                                               std::shared_ptr<AppConfigStore> configStorePtr,
                                               std::shared_ptr<ShellLayoutStore> layoutStorePtr,
                                               std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
                                               GtkUiDependencies dependencies)
    : _runtime{runtime}
    , _parentWindow{window}
    , _registry{}
    , _actionRegistry{}
    , _trackRowCache{dependencies.trackRowCache}
    , _imageCache{dependencies.imageCache}
    , _playbackSequence{dependencies.playbackSequence}
    , _playbackCommandSurface{dependencies.playbackCommandSurface}
    , _tagEditController{dependencies.tagEditController}
    , _importExportActions{dependencies.importExportActions}
    , _trackPageHost{dependencies.trackPageHost}
    , _trackPresentationCatalog{dependencies.trackPresentationCatalog}
    , _trackPresentationPreferences{dependencies.trackPresentationPreferences}
    , _listNavigationController{dependencies.listNavigationController}
    , _createSmartListFromExpression{std::move(dependencies.createSmartListFromExpression)}
    , _menuModelPtr{std::move(dependencies.menuModelPtr)}
    , _host{_registry}
    , _configStorePtr{std::move(configStorePtr)}
    , _layoutStorePtr{std::move(layoutStorePtr)}
    , _componentStateStorePtr{std::move(componentStateStorePtr)}
    , _themeCoordinator{requireThemeCoordinator(dependencies)}
  {
    _runtimeState.componentStateStore = _componentStateStorePtr.get();
    layout::LayoutRuntime::registerStandardComponents(_registry);

    auto const registerAction = [this](std::string_view id,
                                       std::string_view label,
                                       std::string_view category,
                                       uimodel::LayoutActionCapabilities caps,
                                       layout::ActionHandler handler,
                                       layout::ActionStateProvider stateProvider = {})
    {
      _actionRegistry.registerAction(
        uimodel::LayoutActionDescriptor{
          .id = std::string{id}, .label = std::string{label}, .category = std::string{category}, .capabilities = caps},
        std::move(handler),
        std::move(stateProvider));
    };

    auto const hasActiveSequence = [this](layout::ActionActivationContext const&) -> uimodel::LayoutActionAvailability
    {
      if (_playbackSequence != nullptr)
      {
        return uimodel::LayoutActionAvailability{
          .enabled = _playbackSequence->state().currentTrackId != kInvalidTrackId, .disabledReason = ""};
      }

      return uimodel::LayoutActionAvailability{.enabled = false, .disabledReason = ""};
    };

    registerPlaybackActions(registerAction);
    registerShellActions(registerAction);
    registerWorkspaceActions(registerAction, hasActiveSequence);
    registerTrackActions(registerAction);

    _playbackSubs.push_back(
      commandSurface(_playbackCommandSurface).onAvailabilityChanged([this] { refreshExportedActions(); }));
  }

  ShellLayoutController::~ShellLayoutController()
  {
    _tasks.cancelAll();
    _optEditorThemeToken.reset();
    _editorDialogPtr.reset();
    // Components retain LayoutRuntimeState and may flush pending state while
    // destructing, so release them before the state and its store owner.
    _host.clearLayout();
  }

  void ShellLayoutController::setMenuModel(Glib::RefPtr<Gio::MenuModel> menuModelPtr)
  {
    _menuModelPtr = std::move(menuModelPtr);
  }

  void ShellLayoutController::registerPlaybackActions(RegisterActionFn const& registerAction)
  {
    auto const execute = [this](uimodel::PlaybackCommand command)
    {
      return [this, command](layout::ActionActivationContext&)
      { commandSurface(_playbackCommandSurface).execute(command); };
    };

    auto const isEnabled = [this](uimodel::PlaybackCommand command)
    {
      return [this, command](layout::ActionActivationContext const&) -> uimodel::LayoutActionAvailability
      {
        return uimodel::LayoutActionAvailability{
          .enabled = commandSurface(_playbackCommandSurface).isEnabled(command), .disabledReason = ""};
      };
    };

    using Command = uimodel::PlaybackCommand;

    registerAction("playback.play",
                   "Play",
                   "Playback",
                   uimodel::LayoutActionCapability::None,
                   execute(Command::Play),
                   isEnabled(Command::Play));

    registerAction("playback.pause",
                   "Pause",
                   "Playback",
                   uimodel::LayoutActionCapability::None,
                   execute(Command::Pause),
                   isEnabled(Command::Pause));

    registerAction("playback.playPause",
                   "Play/Pause",
                   "Playback",
                   uimodel::LayoutActionCapability::None,
                   execute(Command::PlayPause),
                   isEnabled(Command::PlayPause));

    registerAction("playback.stop",
                   "Stop",
                   "Playback",
                   uimodel::LayoutActionCapability::None,
                   execute(Command::Stop),
                   isEnabled(Command::Stop));

    registerAction("playback.next",
                   "Next",
                   "Playback",
                   uimodel::LayoutActionCapability::None,
                   execute(Command::Next),
                   isEnabled(Command::Next));

    registerAction("playback.previous",
                   "Previous",
                   "Playback",
                   uimodel::LayoutActionCapability::None,
                   execute(Command::Previous),
                   isEnabled(Command::Previous));

    registerAction("playback.toggleShuffle",
                   "Toggle Shuffle",
                   "Playback",
                   uimodel::LayoutActionCapability::None,
                   execute(Command::ToggleShuffle),
                   isEnabled(Command::ToggleShuffle));

    registerAction("playback.cycleRepeat",
                   "Cycle Repeat",
                   "Playback",
                   uimodel::LayoutActionCapability::None,
                   execute(Command::CycleRepeat),
                   isEnabled(Command::CycleRepeat));

    registerAction("playback.showOutputDeviceSelector",
                   "Output Devices",
                   "Playback",
                   uimodel::LayoutActionCapability::RequiresAnchor | uimodel::LayoutActionCapability::PresentsMenu,
                   [](layout::ActionActivationContext& ctx)
                   {
                     auto* const popover = Gtk::make_managed<OutputDevicePopover>(ctx.runtime.playback());
                     popover->set_parent(ctx.anchorWidget);
                     popover->signal_closed().connect([popover] { popover->unparent(); });
                     popover->popup();
                   },
                   {});
  }

  void ShellLayoutController::registerShellActions(RegisterActionFn const& registerAction)
  {
    registerAction("shell.showSystemMenu",
                   "System Menu",
                   "Shell",
                   uimodel::LayoutActionCapability::RequiresAnchor | uimodel::LayoutActionCapability::PresentsMenu,
                   [this](layout::ActionActivationContext& ctx)
                   {
                     if (_menuModelPtr)
                     {
                       auto* const popover = Gtk::make_managed<Gtk::PopoverMenu>(_menuModelPtr);
                       popover->set_parent(ctx.anchorWidget);
                       popover->set_has_arrow(true);
                       popover->signal_closed().connect([popover] { popover->unparent(); });
                       popover->popup();
                     }
                     else
                     {
                       APP_LOG_WARN("shell.showSystemMenu invoked but menuModel is missing");
                     }
                   },
                   {});

    registerAction("shell.showSoul",
                   "Aobus Soul",
                   "Shell",
                   uimodel::LayoutActionCapability::None,
                   [](layout::ActionActivationContext& ctx)
                   {
                     auto* const window = new AobusSoulWindow{};
                     window->set_transient_for(ctx.parentWindow);
                     window->bind(ctx.runtime.playback());
                     window->signal_hide().connect([window] { delete window; });
                     window->present();
                   },
                   {});

    registerAction("shell.editLayout",
                   "Edit Layout",
                   "Shell",
                   uimodel::LayoutActionCapability::None,
                   [this](layout::ActionActivationContext&)
                   {
                     if (_configStorePtr)
                     {
                       openEditor(*_configStorePtr);
                     }
                   },
                   {});
  }

  void ShellLayoutController::registerWorkspaceActions(RegisterActionFn const& registerAction,
                                                       layout::ActionStateProvider const& hasActiveSequence)
  {
    registerAction(
      "workspace.revealCurrentTrack",
      "Reveal Track",
      "Workspace",
      uimodel::LayoutActionCapability::None,
      [](layout::ActionActivationContext& ctx) { ctx.runtime.playback().revealPlayingTrack(); },
      hasActiveSequence);
  }

  void ShellLayoutController::registerTrackActions(RegisterActionFn const& registerAction)
  {
    registerAction(
      "track.presentProperties",
      "Properties",
      "Tracks",
      uimodel::LayoutActionCapability::None,
      [this](layout::ActionActivationContext& ctx)
      {
        if (_tagEditController != nullptr)
        {
          auto const target = rt::FocusedViewTarget{};
          auto projPtr =
            ctx.runtime.views().detailProjection(target, ctx.runtime.workspace(), ctx.runtime.library().changes());

          if (auto const snap = projPtr->snapshot(); !snap.trackIds.empty())
          {
            _tagEditController->presentProperties(
              TrackSelection{.listId = kInvalidListId, .selectedIds = snap.trackIds});
          }
        }
      },
      [](layout::ActionActivationContext const& ctx) -> uimodel::LayoutActionAvailability
      {
        auto const target = rt::FocusedViewTarget{};
        auto projPtr =
          ctx.runtime.views().detailProjection(target, ctx.runtime.workspace(), ctx.runtime.library().changes());
        return uimodel::LayoutActionAvailability{
          .enabled = !projPtr->snapshot().trackIds.empty(), .disabledReason = ""};
      });

    registerAction(
      "track.editTags",
      "Edit Tags",
      "Tracks",
      uimodel::LayoutActionCapability::RequiresAnchor | uimodel::LayoutActionCapability::PresentsMenu,
      [this](layout::ActionActivationContext& ctx)
      {
        if (_tagEditController != nullptr)
        {
          auto const target = rt::FocusedViewTarget{};
          auto projPtr =
            ctx.runtime.views().detailProjection(target, ctx.runtime.workspace(), ctx.runtime.library().changes());

          if (auto const snap = projPtr->snapshot(); !snap.trackIds.empty())
          {
            _tagEditController->openTagEditor(
              TrackSelection{.listId = kInvalidListId, .selectedIds = snap.trackIds}, ctx.anchorWidget);
          }
        }
      },
      [](layout::ActionActivationContext const& ctx) -> uimodel::LayoutActionAvailability
      {
        auto const target = rt::FocusedViewTarget{};
        auto projPtr =
          ctx.runtime.views().detailProjection(target, ctx.runtime.workspace(), ctx.runtime.library().changes());
        return uimodel::LayoutActionAvailability{
          .enabled = !projPtr->snapshot().trackIds.empty(), .disabledReason = ""};
      });
  }

  void ShellLayoutController::attachToWindow()
  {
    _parentWindow.set_child(_host);

    if (auto* actionMap = dynamic_cast<Gio::ActionMap*>(&_parentWindow); actionMap != nullptr)
    {
      _gioBridgeSessionPtr = layout::GioActionBridge::exportActions(_actionRegistry, *actionMap, *this);
    }
    else
    {
      APP_LOG_WARN("ShellLayoutController parentWindow is not a Gio::ActionMap. Skipping action export.");
    }
  }

  void ShellLayoutController::refreshExportedActions()
  {
    if (_gioBridgeSessionPtr)
    {
      _gioBridgeSessionPtr->refreshStates();
    }
  }

  void ShellLayoutController::rebuildHost(uimodel::LayoutDocument const& doc)
  {
    auto const dependencies = GtkUiDependencies{
      .trackRowCache = _trackRowCache,
      .imageCache = _imageCache,
      .playbackSequence = _playbackSequence,
      .playbackCommandSurface = _playbackCommandSurface,
      .tagEditController = _tagEditController,
      .importExportActions = _importExportActions,
      .trackPageHost = _trackPageHost,
      .trackPresentationCatalog = _trackPresentationCatalog,
      .trackPresentationPreferences = _trackPresentationPreferences,
      .listNavigationController = _listNavigationController,
      .themeCoordinator = &_themeCoordinator,
      .createSmartListFromExpression = _createSmartListFromExpression,
      .menuModelPtr = _menuModelPtr,
    };
    auto ctx = layout::LayoutBuildContext{.registry = _registry,
                                          .actionRegistry = _actionRegistry,
                                          .runtime = _runtime,
                                          .parentWindow = _parentWindow,
                                          .runtimeState = _runtimeState,
                                          .dependencies = dependencies};
    _host.setLayout(ctx, doc);
  }

  void ShellLayoutController::loadLayout(AppConfigStore& /*configStore*/)
  {
    auto& runtime = _runtime.async();
    runtime.spawnWithLifetime(
      &_tasks,
      [self = this,
       storePtr = _layoutStorePtr,
       componentStateStorePtr = _componentStateStorePtr,
       configStorePtr = _configStorePtr](std::stop_token const stopToken) mutable
      {
        return self->loadLayoutWorkflow(
          std::move(storePtr), std::move(componentStateStorePtr), std::move(configStorePtr), stopToken);
      });
  }

  async::Task<void> ShellLayoutController::loadLayoutWorkflow(
    std::shared_ptr<ShellLayoutStore> layoutStorePtr,
    std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
    std::shared_ptr<AppConfigStore> configStorePtr,
    std::stop_token const stopToken)
  {
    APP_LOG_DEBUG("ShellLayoutController: loadLayout coroutine started on UI thread");

    auto& asyncRuntime = _runtime.async();
    auto optResult = std::optional<LayoutLoadResult>{};
    bool failed = false;

    try
    {
      co_await asyncRuntime.resumeOnWorker(stopToken);
      APP_LOG_DEBUG("ShellLayoutController: loading layout config on background worker thread");

      if (layoutStorePtr && configStorePtr)
      {
        optResult = loadLayoutOnWorker(*layoutStorePtr, componentStateStorePtr.get(), *configStorePtr);
      }
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      asyncRuntime.reportUnhandledException(std::current_exception(), "shell layout load workflow");
      failed = true;
    }

    co_await asyncRuntime.resumeOnCallbackExecutor(stopToken);

    if (failed)
    {
      co_return;
    }

    if (!optResult)
    {
      co_return;
    }

    APP_LOG_DEBUG("ShellLayoutController: resumed on UI thread, applying layout");
    applyLoadedLayoutWithFaultReporting(
      std::move(optResult->presetId), std::move(optResult->document), std::move(optResult->componentState));
  }

  void ShellLayoutController::applyLoadedLayoutWithFaultReporting(std::string presetId,
                                                                  uimodel::LayoutDocument document,
                                                                  uimodel::LayoutComponentStateDocument componentState)
  {
    try
    {
      applyLoadedLayout(std::move(presetId), std::move(document), std::move(componentState));
    }
    catch (...)
    {
      async::rethrowIfOperationCancelled();
      _runtime.async().reportUnhandledException(std::current_exception(), "shell layout apply workflow");
    }
  }

  void ShellLayoutController::applyLoadedLayout(std::string presetId,
                                                uimodel::LayoutDocument document,
                                                uimodel::LayoutComponentStateDocument componentState)
  {
    _session.applyLoadedLayout(std::move(presetId), std::move(document));
    auto const snapshot = _session.snapshot();
    _runtimeState.activePresetId = snapshot.presetId;
    _runtimeState.componentState = std::move(componentState);

    for (auto const& diagnostic : uimodel::validateStatefulLayoutNodeIds(snapshot.layout))
    {
      if (diagnostic.severity == uimodel::LayoutNodeIdDiagnosticSeverity::Error)
      {
        APP_LOG_ERROR("ShellLayoutController: Layout id error in preset '{}' component '{}' ({}): {}",
                      snapshot.presetId,
                      diagnostic.componentId,
                      diagnostic.componentType,
                      diagnostic.message);
      }
      else
      {
        APP_LOG_WARN("ShellLayoutController: Layout id warning in preset '{}' component '{}' ({}): {}",
                     snapshot.presetId,
                     diagnostic.componentId,
                     diagnostic.componentType,
                     diagnostic.message);
      }
    }

    rebuildHost(snapshot.layout);
  }

  void ShellLayoutController::openEditor(AppConfigStore& configStore)
  {
    auto prefs = rt::AppPrefsState{};
    configStore.loadAppPrefs(prefs);

    auto const initialPresetId =
      uimodel::ShellLayoutSessionModel::activeOrDefaultPresetId(_session.snapshot().presetId);
    auto const initialThemeId = std::string{rt::themePresetToString(_themeCoordinator.activeTheme())};

    auto loader = [storePtr = _layoutStorePtr](std::string_view id) -> uimodel::LayoutDocument
    {
      if (storePtr)
      {
        if (auto optDoc = storePtr->load(id); optDoc)
        {
          return std::move(*optDoc);
        }
      }

      return layout::makeBuiltInLayout(layout::presetIdFromString(id));
    };

    _editorDialogPtr = std::make_shared<layout::editor::LayoutEditorDialog>(dynamic_cast<Gtk::Window&>(_parentWindow),
                                                                            _registry,
                                                                            _actionRegistry,
                                                                            _session.snapshot().layout,
                                                                            initialPresetId,
                                                                            initialThemeId,
                                                                            std::move(loader));
    auto* const dialogRaw = _editorDialogPtr.get();

    _optEditorThemeToken = _themeCoordinator.registerToplevel(*dialogRaw);

    _runtimeState.editMode = true;
    _runtimeState.onNodeMoved = [weakDialogPtr = std::weak_ptr{_editorDialogPtr}](
                                  std::string const& nodeId, std::int32_t xPosition, std::int32_t yPosition)
    {
      if (auto const sharedDialogPtr = weakDialogPtr.lock(); sharedDialogPtr != nullptr)
      {
        sharedDialogPtr->updateNodePosition(nodeId, xPosition, yPosition);
      }
    };

    rebuildHost(_session.snapshot().layout);

    dialogRaw->signalApplyPreview().connect([this](uimodel::LayoutDocument const& doc) { rebuildHost(doc); });

    dialogRaw->signalThemePreview().connect([this](std::string_view themeId)
                                            { _themeCoordinator.setTheme(rt::themePresetFromString(themeId)); });

    dialogRaw->signalSaveRequest().connect([this](layout::editor::LayoutSaveResult const& result)
                                           { this->handleEditorSaveRequested(result); });

    dialogRaw->signal_hide().connect(
      [this]
      {
        _runtimeState.editMode = false;
        _runtimeState.onNodeMoved = nullptr;
        _optEditorThemeToken.reset();
        _editorDialogPtr.reset();
      });

    dialogRaw->signal_response().connect(
      [this, oldTheme = _themeCoordinator.activeTheme()](std::int32_t responseId)
      {
        if (responseId == Gtk::ResponseType::CANCEL)
        {
          rebuildHost(_session.snapshot().layout);
          _themeCoordinator.setTheme(oldTheme);
        }
      });

    dialogRaw->present();
  }

  void ShellLayoutController::handleEditorSaveRequested(layout::editor::LayoutSaveResult const& result)
  {
    if (_layoutStorePtr)
    {
      for (auto const& [id, doc] : result.modified)
      {
        _layoutStorePtr->save(doc, id);
      }

      for (auto const& id : result.resets)
      {
        _layoutStorePtr->remove(id);
      }
    }

    if (_componentStateStorePtr)
    {
      for (auto const& [id, doc] : result.modified)
      {
        if (!_componentStateStorePtr->prune(id, doc))
        {
          APP_LOG_WARN("ShellLayoutController: Failed to prune runtime state for preset '{}'", id);
        }
      }

      for (auto const& id : result.resets)
      {
        if (!_componentStateStorePtr->removePreset(id))
        {
          APP_LOG_WARN("ShellLayoutController: Failed to remove runtime state for preset '{}'", id);
        }
      }
    }

    _session.applyEditorSave(result.activePresetId, result.activeDocument);
    auto const snapshot = _session.snapshot();
    _runtimeState.activePresetId = snapshot.presetId;
    _runtimeState.componentState =
      _componentStateStorePtr == nullptr
        ? uimodel::ShellLayoutSessionModel::emptyComponentState(snapshot.presetId)
        : _componentStateStorePtr->load(snapshot.presetId)
            .value_or(uimodel::ShellLayoutSessionModel::emptyComponentState(snapshot.presetId));

    if (_configStorePtr)
    {
      auto prefsUpdate = rt::AppPrefsState{};
      _configStorePtr->loadAppPrefs(prefsUpdate);
      prefsUpdate.lastLayoutPreset = snapshot.presetId;
      _configStorePtr->saveAppPrefs(prefsUpdate);
      _themeCoordinator.setTheme(rt::themePresetFromString(prefsUpdate.lastThemePreset));
    }

    rebuildHost(snapshot.layout);
  }

  void ShellLayoutController::resetRuntimeLayoutState()
  {
    auto reset = _session.resetRuntimeLayoutState();

    if (_componentStateStorePtr)
    {
      if (!_componentStateStorePtr->removePreset(reset.presetId))
      {
        APP_LOG_WARN("ShellLayoutController: Failed to remove runtime state for preset '{}'", reset.presetId);
      }
    }

    _runtimeState.activePresetId = reset.presetId;
    _runtimeState.componentState = std::move(reset.componentState);
    rebuildHost(_session.snapshot().layout);
    refreshExportedActions();
  }

  void ShellLayoutController::saveCurrentPanelSizesAsLayoutDefaults()
  {
    auto optPromotion = _session.preparePanelSizePromotion(_runtimeState.componentState);

    if (!optPromotion)
    {
      auto const presetId = uimodel::ShellLayoutSessionModel::activeOrDefaultPresetId(_session.snapshot().presetId);
      APP_LOG_INFO("ShellLayoutController: No panel sizes to promote for preset '{}'", presetId);
      return;
    }

    auto presetId = optPromotion->presetId;
    auto apply = [this, presetId, promotion = std::move(*optPromotion)](bool confirmed) mutable
    {
      if (!confirmed)
      {
        APP_LOG_INFO("ShellLayoutController: User cancelled promoting panel sizes for preset '{}'", presetId);
        return;
      }

      applyPromotedPanelSizes(presetId, std::move(promotion.layout), std::move(promotion.componentState));
    };

    if (_confirmPromotionFn)
    {
      _confirmPromotionFn(presetId, std::move(apply));
    }
    else
    {
      apply(true);
    }
  }

  void ShellLayoutController::applyPromotedPanelSizes(std::string const& presetId,
                                                      uimodel::LayoutDocument promotedLayout,
                                                      uimodel::LayoutComponentStateDocument promotedState)
  {
    if (_layoutStorePtr)
    {
      _layoutStorePtr->save(promotedLayout, presetId);
      APP_LOG_INFO("ShellLayoutController: Promoted panel sizes to layout defaults for preset '{}'", presetId);
    }

    if (_componentStateStorePtr)
    {
      if (promotedState.components.empty())
      {
        if (!_componentStateStorePtr->removePreset(presetId))
        {
          APP_LOG_WARN("ShellLayoutController: Failed to remove runtime state for preset '{}'", presetId);
        }
      }
      else
      {
        _componentStateStorePtr->save(presetId, promotedState);
      }
    }

    _session.applyPanelSizePromotion(uimodel::ShellLayoutPanelSizePromotion{
      .presetId = presetId, .layout = std::move(promotedLayout), .componentState = promotedState});
    auto const snapshot = _session.snapshot();
    _runtimeState.activePresetId = snapshot.presetId;
    _runtimeState.componentState = std::move(promotedState);
    rebuildHost(snapshot.layout);
    refreshExportedActions();
  }

  void ShellLayoutController::setConfirmPromotionCallback(ConfirmPromotionFn fn)
  {
    _confirmPromotionFn = std::move(fn);
  }

  uimodel::LayoutActionActivationResult ShellLayoutController::activateAction(std::string_view id)
  {
    auto ctx = actionContext(id);
    return _actionRegistry.tryActivate(id, ctx);
  }

  uimodel::LayoutActionAvailability ShellLayoutController::actionAvailability(std::string_view id)
  {
    auto ctx = actionContext(id);
    return _actionRegistry.state(id, ctx);
  }

  layout::ActionActivationContext ShellLayoutController::actionContext(std::string_view componentId)
  {
    return layout::ActionActivationContext{.runtime = _runtime,
                                           .parentWindow = _parentWindow,
                                           .anchorWidget = _parentWindow,
                                           .componentId = std::string{componentId}};
  }

  bool ShellLayoutController::canProvideSafeAnchor(uimodel::LayoutActionDescriptor const& desc) const
  {
    // The parent window is a safe fallback anchor ONLY for specific shell actions.
    // E.g., shell.showSystemMenu can be safely opened relative to the main window.
    return desc.id == "shell.showSystemMenu";
  }
} // namespace ao::gtk
