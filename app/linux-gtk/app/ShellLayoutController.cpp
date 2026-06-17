// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellLayoutController.h"

#include "AppConfig.h"
#include "ShellLayoutComponentStateStore.h"
#include "ShellLayoutStore.h"
#include "app/ThemeCoordinator.h"
#include "app/ThemePreset.h"
#include "layout/document/GtkLayoutPresets.h"
#include "layout/document/LayoutDocument.h"
#include "layout/editor/LayoutEditorDialog.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/GioActionBridge.h"
#include "layout/runtime/LayoutContext.h"
#include "layout/runtime/LayoutHost.h"
#include "layout/runtime/LayoutRuntime.h"
#include "layout/state/LayoutComponentState.h"
#include "layout/state/LayoutNodeId.h"
#include "layout/state/LayoutStatePromoter.h"
#include "playback/AobusSoulWindow.h"
#include "playback/AudioDeviceSelector.h"
#include "tag/TagEditController.h"
#include <ao/Type.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/audio/Types.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ProjectionTypes.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/TrackDetailProjection.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/uimodel/playback/PlaybackQueueModel.h>
#include <ao/utility/Log.h>

#include <gtkmm/dialog.h>
#include <gtkmm/object.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>
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
      layout::LayoutDocument document;
      layout::LayoutComponentStateDocument componentState;
    };

    layout::LayoutComponentStateDocument emptyComponentState(std::string_view presetId)
    {
      return layout::LayoutComponentStateDocument{.preset = std::string{presetId}};
    }

    LayoutLoadResult loadLayoutOnWorker(ShellLayoutStore& store,
                                        ShellLayoutComponentStateStore* componentStateStore,
                                        AppConfig& config)
    {
      auto prefs = rt::AppPrefsState{};
      config.loadAppPrefs(prefs);

      auto presetId = layout::LayoutPresetId::Classic;
      auto presetIdStr = std::string{"classic"};

      if (prefs.lastLayoutPreset == "modern")
      {
        presetId = layout::LayoutPresetId::Modern;
        presetIdStr = "modern";
      }
      else if (!prefs.lastLayoutPreset.empty() && prefs.lastLayoutPreset != "classic")
      {
        APP_LOG_DEBUG(
          "ShellLayoutController: Unknown layout preset '{}', falling back to classic", prefs.lastLayoutPreset);
      }

      auto optDoc = store.load(presetIdStr);
      auto doc = optDoc ? std::move(*optDoc) : layout::createBuiltInLayout(presetId);
      auto stateDoc = componentStateStore == nullptr
                        ? emptyComponentState(presetIdStr)
                        : componentStateStore->load(presetIdStr).value_or(emptyComponentState(presetIdStr));

      return {.presetId = presetIdStr, .document = std::move(doc), .componentState = std::move(stateDoc)};
    }
  } // namespace
  ShellLayoutController::ShellLayoutController(rt::AppRuntime& runtime,
                                               Gtk::Window& window,
                                               std::shared_ptr<AppConfig> configPtr,
                                               std::shared_ptr<ShellLayoutStore> layoutStorePtr,
                                               std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
                                               ThemeCoordinator& themeCoordinator)
    : _registry{}
    , _actionRegistry{}
    , _context{.registry = _registry, .actionRegistry = _actionRegistry, .runtime = runtime, .parentWindow = window}
    , _host{_registry}
    , _configPtr{std::move(configPtr)}
    , _layoutStorePtr{std::move(layoutStorePtr)}
    , _componentStateStorePtr{std::move(componentStateStorePtr)}
    , _themeCoordinator{themeCoordinator}
  {
    _context.componentStateStore = _componentStateStorePtr.get();
    layout::LayoutRuntime::registerStandardComponents(_registry);

    auto const refreshActionStates = [this]
    {
      if (_gioBridgeSessionPtr)
      {
        _gioBridgeSessionPtr->refreshStates();
      }
    };

    _playbackSubs.push_back(_context.runtime.playback().onStarted(refreshActionStates));
    _playbackSubs.push_back(_context.runtime.playback().onStopped(refreshActionStates));
    _playbackSubs.push_back(
      _context.runtime.playback().onNowPlayingChanged([refreshActionStates](auto const&) { refreshActionStates(); }));
    _playbackSubs.push_back(
      _context.runtime.playback().onShuffleModeChanged([refreshActionStates](auto const&) { refreshActionStates(); }));
    _playbackSubs.push_back(
      _context.runtime.playback().onRepeatModeChanged([refreshActionStates](auto const&) { refreshActionStates(); }));

    auto const registerAction = [this](std::string_view id,
                                       std::string_view label,
                                       std::string_view category,
                                       layout::ActionCapabilities caps,
                                       layout::ActionHandler handler,
                                       layout::ActionStateProvider stateProvider = {})
    {
      _actionRegistry.registerAction(
        layout::ActionDescriptor{
          .id = std::string{id}, .label = std::string{label}, .category = std::string{category}, .capabilities = caps},
        std::move(handler),
        std::move(stateProvider));
    };

    auto const hasActiveQueue = [this](layout::ActionActivationContext const&) -> layout::ActionState
    {
      if (auto* const queueModel = _context.playback.queueModel; queueModel != nullptr)
      {
        return layout::ActionState{.enabled = queueModel->isActive(), .disabledReason = ""};
      }

      return layout::ActionState{.enabled = false, .disabledReason = ""};
    };

    registerPlaybackActions(registerAction, hasActiveQueue);
    registerShellActions(registerAction);
    registerWorkspaceActions(registerAction, hasActiveQueue);
    registerTrackActions(registerAction);
  }

  void ShellLayoutController::registerPlaybackActions(RegisterActionFn const& registerAction,
                                                      layout::ActionStateProvider const& hasActiveQueue)
  {
    registerAction("playback.playPause",
                   "Play/Pause",
                   "Playback",
                   layout::ActionCapability::None,
                   [](layout::ActionActivationContext& ctx)
                   {
                     if (auto const& state = ctx.runtime.playback().state();
                         state.transport == audio::Transport::Paused)
                     {
                       ctx.runtime.playback().resume();
                     }
                     else if (state.transport == audio::Transport::Playing)
                     {
                       ctx.runtime.playback().pause();
                     }
                     else
                     {
                       ctx.runtime.playSelectionInFocusedView();
                     }
                   },
                   {});

    registerAction(
      "playback.stop",
      "Stop",
      "Playback",
      layout::ActionCapability::None,
      [](layout::ActionActivationContext& ctx) { ctx.runtime.playback().stop(); },
      hasActiveQueue);

    registerAction(
      "playback.next",
      "Next",
      "Playback",
      layout::ActionCapability::None,
      [this](layout::ActionActivationContext&)
      {
        if (auto* const queueModel = _context.playback.queueModel; queueModel != nullptr)
        {
          queueModel->next();
        }
      },
      [this](layout::ActionActivationContext const&) -> layout::ActionState
      {
        if (auto* const queueModel = _context.playback.queueModel; queueModel != nullptr)
        {
          return layout::ActionState{.enabled = queueModel->hasNext(), .disabledReason = ""};
        }

        return layout::ActionState{.enabled = false, .disabledReason = ""};
      });

    registerAction(
      "playback.previous",
      "Previous",
      "Playback",
      layout::ActionCapability::None,
      [this](layout::ActionActivationContext&)
      {
        if (auto* const queueModel = _context.playback.queueModel; queueModel != nullptr)
        {
          queueModel->previous();
        }
      },
      [this](layout::ActionActivationContext const&) -> layout::ActionState
      {
        if (auto* const queueModel = _context.playback.queueModel; queueModel != nullptr)
        {
          return layout::ActionState{.enabled = queueModel->hasPrevious(), .disabledReason = ""};
        }

        return layout::ActionState{.enabled = false, .disabledReason = ""};
      });

    registerAction(
      "playback.toggleShuffle",
      "Toggle Shuffle",
      "Playback",
      layout::ActionCapability::None,
      [this](layout::ActionActivationContext& ctx)
      {
        if (auto* const queueModel = _context.playback.queueModel; queueModel != nullptr)
        {
          auto const current = ctx.runtime.playback().state().shuffleMode;
          auto const next = (current == rt::ShuffleMode::Off) ? rt::ShuffleMode::On : rt::ShuffleMode::Off;
          queueModel->setShuffleMode(next);
        }
      },
      hasActiveQueue);

    registerAction(
      "playback.cycleRepeat",
      "Cycle Repeat",
      "Playback",
      layout::ActionCapability::None,
      [this](layout::ActionActivationContext& ctx)
      {
        if (auto* const queueModel = _context.playback.queueModel; queueModel != nullptr)
        {
          auto const current = ctx.runtime.playback().state().repeatMode;
          auto next = rt::RepeatMode::Off;

          if (current == rt::RepeatMode::Off)
          {
            next = rt::RepeatMode::All;
          }
          else if (current == rt::RepeatMode::All)
          {
            next = rt::RepeatMode::One;
          }

          queueModel->setRepeatMode(next);
        }
      },
      hasActiveQueue);

    registerAction("playback.showAudioDeviceSelector",
                   "Audio Devices",
                   "Playback",
                   layout::ActionCapability::RequiresAnchor | layout::ActionCapability::PresentsMenu,
                   [](layout::ActionActivationContext& ctx)
                   {
                     auto* const popover = Gtk::make_managed<AudioDeviceSelector>(ctx.runtime.playback());
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
                   layout::ActionCapability::RequiresAnchor | layout::ActionCapability::PresentsMenu,
                   [this](layout::ActionActivationContext& ctx)
                   {
                     if (auto const menuPtr = _context.shell.menuModelPtr; menuPtr)
                     {
                       auto* const popover = Gtk::make_managed<Gtk::PopoverMenu>(menuPtr);
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
                   layout::ActionCapability::None,
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
                   layout::ActionCapability::None,
                   [this](layout::ActionActivationContext&)
                   {
                     if (_configPtr)
                     {
                       openEditor(*_configPtr);
                     }
                   },
                   {});
  }

  void ShellLayoutController::registerWorkspaceActions(RegisterActionFn const& registerAction,
                                                       layout::ActionStateProvider const& hasActiveQueue)
  {
    registerAction(
      "workspace.revealCurrentTrack",
      "Reveal Track",
      "Workspace",
      layout::ActionCapability::None,
      [](layout::ActionActivationContext& ctx) { ctx.runtime.playback().revealPlayingTrack(); },
      hasActiveQueue);
  }

  void ShellLayoutController::registerTrackActions(RegisterActionFn const& registerAction)
  {
    registerAction(
      "track.showProperties",
      "Properties",
      "Tracks",
      layout::ActionCapability::None,
      [this](layout::ActionActivationContext& ctx)
      {
        if (auto* tagController = _context.tag.editController; tagController != nullptr)
        {
          auto const target = rt::FocusedViewTarget{};
          auto proj = rt::TrackDetailProjection{
            target, ctx.runtime.views(), ctx.runtime.musicLibrary(), ctx.runtime.workspace(), ctx.runtime.mutation()};

          if (auto const snap = proj.snapshot(); !snap.trackIds.empty())
          {
            tagController->showProperties(
              TrackSelectionContext{.listId = kInvalidListId, .selectedIds = snap.trackIds});
          }
        }
      },
      [](layout::ActionActivationContext const& ctx) -> layout::ActionState
      {
        auto const target = rt::FocusedViewTarget{};
        auto proj = rt::TrackDetailProjection{
          target, ctx.runtime.views(), ctx.runtime.musicLibrary(), ctx.runtime.workspace(), ctx.runtime.mutation()};
        return layout::ActionState{.enabled = !proj.snapshot().trackIds.empty(), .disabledReason = ""};
      });

    registerAction(
      "track.editTags",
      "Edit Tags",
      "Tracks",
      layout::ActionCapability::RequiresAnchor | layout::ActionCapability::PresentsMenu,
      [this](layout::ActionActivationContext& ctx)
      {
        if (auto* tagController = _context.tag.editController; tagController != nullptr)
        {
          auto const target = rt::FocusedViewTarget{};
          auto proj = rt::TrackDetailProjection{
            target, ctx.runtime.views(), ctx.runtime.musicLibrary(), ctx.runtime.workspace(), ctx.runtime.mutation()};

          if (auto const snap = proj.snapshot(); !snap.trackIds.empty())
          {
            tagController->showTagEditor(
              TrackSelectionContext{.listId = kInvalidListId, .selectedIds = snap.trackIds}, ctx.anchorWidget);
          }
        }
      },
      [](layout::ActionActivationContext const& ctx) -> layout::ActionState
      {
        auto const target = rt::FocusedViewTarget{};
        auto proj = rt::TrackDetailProjection{
          target, ctx.runtime.views(), ctx.runtime.musicLibrary(), ctx.runtime.workspace(), ctx.runtime.mutation()};
        return layout::ActionState{.enabled = !proj.snapshot().trackIds.empty(), .disabledReason = ""};
      });
  }

  void ShellLayoutController::attachToWindow()
  {
    _context.parentWindow.set_child(_host);

    if (auto* actionMap = dynamic_cast<Gio::ActionMap*>(&_context.parentWindow); actionMap != nullptr)
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

  void ShellLayoutController::loadLayout(AppConfig& /*config*/)
  {
    auto& runtime = _context.runtime.async();
    runtime.spawnWithLifetime(&_tasks,
                              [](ShellLayoutController* self,
                                 std::shared_ptr<ShellLayoutStore> storePtr,
                                 std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
                                 std::shared_ptr<AppConfig> configPtr) -> async::Task<void>
                              {
                                APP_LOG_DEBUG("ShellLayoutController: loadLayout coroutine started on UI thread");

                                auto* const asyncRuntime = &self->_context.runtime.async();
                                co_await asyncRuntime->resumeOnWorker();
                                APP_LOG_DEBUG("ShellLayoutController: loading config on background worker thread");

                                if (storePtr && configPtr)
                                {
                                  auto result = loadLayoutOnWorker(*storePtr, componentStateStorePtr.get(), *configPtr);

                                  co_await asyncRuntime->resumeOnCallbackExecutor();
                                  APP_LOG_DEBUG("ShellLayoutController: resumed on UI thread, applying layout");

                                  self->applyLoadedLayout(std::move(result.presetId),
                                                          std::move(result.document),
                                                          std::move(result.componentState));
                                }
                              }(this, _layoutStorePtr, _componentStateStorePtr, _configPtr));
  }

  void ShellLayoutController::applyLoadedLayout(std::string presetId,
                                                layout::LayoutDocument document,
                                                layout::LayoutComponentStateDocument componentState)
  {
    _activePresetId = std::move(presetId);
    _activeLayout = std::move(document);
    _context.activePresetId = _activePresetId;
    _context.componentState = std::move(componentState);

    for (auto const& diagnostic : layout::validateStatefulLayoutNodeIds(_activeLayout))
    {
      if (diagnostic.severity == layout::LayoutNodeIdDiagnosticSeverity::Error)
      {
        APP_LOG_ERROR("ShellLayoutController: Layout id error in preset '{}' component '{}' ({}): {}",
                      _activePresetId,
                      diagnostic.componentId,
                      diagnostic.componentType,
                      diagnostic.message);
      }
      else
      {
        APP_LOG_WARN("ShellLayoutController: Layout id warning in preset '{}' component '{}' ({}): {}",
                     _activePresetId,
                     diagnostic.componentId,
                     diagnostic.componentType,
                     diagnostic.message);
      }
    }

    _host.setLayout(_context, _activeLayout);
  }

  void ShellLayoutController::openEditor(AppConfig& config)
  {
    auto prefs = rt::AppPrefsState{};
    config.loadAppPrefs(prefs);

    auto const initialPresetId = _activePresetId.empty() ? "classic" : _activePresetId;
    auto const initialThemeId = std::string{themePresetToString(_themeCoordinator.activeTheme())};

    auto loader = [storePtr = _layoutStorePtr](std::string_view id) -> layout::LayoutDocument
    {
      if (storePtr)
      {
        return storePtr->load(id).value_or(layout::createBuiltInLayout(layout::presetIdFromString(id)));
      }

      return layout::createBuiltInLayout(layout::presetIdFromString(id));
    };

    _editorDialogPtr =
      std::make_shared<layout::editor::LayoutEditorDialog>(dynamic_cast<Gtk::Window&>(_context.parentWindow),
                                                           _registry,
                                                           _actionRegistry,
                                                           _activeLayout,
                                                           initialPresetId,
                                                           initialThemeId,
                                                           std::move(loader));
    auto* const dialogRaw = _editorDialogPtr.get();

    _optEditorThemeToken = _themeCoordinator.registerToplevel(*dialogRaw);

    _context.editMode = true;
    _context.onNodeMoved = [weakDialogPtr = std::weak_ptr{_editorDialogPtr}](
                             std::string const& nodeId, std::int32_t posX, std::int32_t posY)
    {
      if (auto const sharedDialogPtr = weakDialogPtr.lock(); sharedDialogPtr != nullptr)
      {
        sharedDialogPtr->updateNodePosition(nodeId, posX, posY);
      }
    };

    _host.setLayout(_context, _activeLayout);

    dialogRaw->signalApplyPreview().connect([this](layout::LayoutDocument const& doc)
                                            { _host.setLayout(_context, doc); });

    dialogRaw->signalThemePreview().connect([this](std::string_view themeId)
                                            { _themeCoordinator.setTheme(themePresetFromString(themeId)); });

    dialogRaw->signalSaveRequest().connect([this](layout::editor::LayoutSaveResult const& result)
                                           { this->onEditorSaveRequest(result); });

    dialogRaw->signal_hide().connect(
      [this]
      {
        _context.editMode = false;
        _context.onNodeMoved = nullptr;
        _optEditorThemeToken.reset();
        _editorDialogPtr.reset();
      });

    dialogRaw->signal_response().connect(
      [this, oldTheme = _themeCoordinator.activeTheme()](std::int32_t responseId)
      {
        if (responseId == Gtk::ResponseType::CANCEL)
        {
          _host.setLayout(_context, _activeLayout);
          _themeCoordinator.setTheme(oldTheme);
        }
      });

    dialogRaw->present();
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void ShellLayoutController::onEditorSaveRequest(layout::editor::LayoutSaveResult const& result)
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

    _activeLayout = result.activeDocument;
    _activePresetId = result.activePresetId;
    _context.activePresetId = _activePresetId;
    _context.componentState =
      _componentStateStorePtr == nullptr
        ? emptyComponentState(_activePresetId)
        : _componentStateStorePtr->load(_activePresetId).value_or(emptyComponentState(_activePresetId));

    if (_configPtr)
    {
      auto prefsUpdate = rt::AppPrefsState{};
      _configPtr->loadAppPrefs(prefsUpdate);
      prefsUpdate.lastLayoutPreset = _activePresetId;

      if (_editorDialogPtr)
      {
        if (auto const themeIdStr = _editorDialogPtr->selectedThemeId(); !themeIdStr.empty())
        {
          _themeCoordinator.setTheme(themePresetFromString(themeIdStr));
          prefsUpdate.lastThemePreset = themeIdStr;
        }
      }

      _configPtr->saveAppPrefs(prefsUpdate);
    }

    _host.setLayout(_context, _activeLayout);
  }

  void ShellLayoutController::resetRuntimeLayoutState()
  {
    auto const presetId = _activePresetId.empty() ? std::string{"classic"} : _activePresetId;

    if (_componentStateStorePtr)
    {
      if (!_componentStateStorePtr->removePreset(presetId))
      {
        APP_LOG_WARN("ShellLayoutController: Failed to remove runtime state for preset '{}'", presetId);
      }
    }

    _context.activePresetId = presetId;
    _context.componentState = emptyComponentState(presetId);
    _host.setLayout(_context, _activeLayout);
    refreshExportedActions();
  }

  void ShellLayoutController::saveCurrentPanelSizesAsLayoutDefaults()
  {
    auto const presetId = _activePresetId.empty() ? std::string{"classic"} : _activePresetId;
    auto promotedLayout = _activeLayout;
    auto promotedState = _context.componentState;
    promotedState.preset = presetId;

    auto const promotion = layout::promotePanelSizeDefaults(promotedLayout, promotedState);

    if (!promotion.changed)
    {
      APP_LOG_INFO("ShellLayoutController: No panel sizes to promote for preset '{}'", presetId);
      return;
    }

    auto apply = [this, presetId, promotedLayout = std::move(promotedLayout), promotedState = std::move(promotedState)](
                   bool confirmed) mutable
    {
      if (!confirmed)
      {
        APP_LOG_INFO("ShellLayoutController: User cancelled promoting panel sizes for preset '{}'", presetId);
        return;
      }

      applyPromotedPanelSizes(presetId, std::move(promotedLayout), std::move(promotedState));
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
                                                      layout::LayoutDocument promotedLayout,
                                                      layout::LayoutComponentStateDocument promotedState)
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
        _componentStateStorePtr->save(promotedState, presetId);
      }
    }

    _activePresetId = presetId;
    _activeLayout = std::move(promotedLayout);
    _context.activePresetId = _activePresetId;
    _context.componentState = std::move(promotedState);
    _host.setLayout(_context, _activeLayout);
    refreshExportedActions();
  }

  void ShellLayoutController::setConfirmPromotionCallback(ConfirmPromotionFn fn)
  {
    _confirmPromotionFn = std::move(fn);
  }

  layout::ActionActivationOutcome ShellLayoutController::activateAction(std::string_view id)
  {
    auto ctx = getActionContext(id);
    return _actionRegistry.tryActivate(id, ctx);
  }

  layout::ActionActivationContext ShellLayoutController::getActionContext(std::string_view componentId)
  {
    return layout::ActionActivationContext{.runtime = _context.runtime,
                                           .parentWindow = _context.parentWindow,
                                           .anchorWidget = _context.parentWindow,
                                           .componentId = std::string{componentId}};
  }

  bool ShellLayoutController::canProvideSafeAnchor(layout::ActionDescriptor const& desc) const
  {
    // The parent window is a safe fallback anchor ONLY for specific shell actions.
    // E.g., shell.showSystemMenu can be safely opened relative to the main window.
    return desc.id == "shell.showSystemMenu";
  }
} // namespace ao::gtk
