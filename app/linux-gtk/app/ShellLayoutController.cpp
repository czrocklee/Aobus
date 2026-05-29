// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellLayoutController.h"

#include "AppConfig.h"
#include "layout/document/LayoutDocument.h"
#include "layout/editor/LayoutEditorDialog.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/GioActionBridge.h"
#include "layout/runtime/LayoutContext.h"
#include "layout/runtime/LayoutHost.h"
#include "layout/runtime/LayoutRuntime.h"
#include "playback/AobusSoulWindow.h"
#include "playback/AudioDeviceSelector.h"
#include <ao/audio/Types.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/async/Runtime.h>
#include <ao/rt/async/Task.h>
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
  ShellLayoutController::ShellLayoutController(rt::AppRuntime& runtime,
                                               Gtk::Window& parentWindow,
                                               std::shared_ptr<AppConfig> config)
    : _registry{}
    , _actionRegistry{}
    , _context{.registry = _registry,
               .actionRegistry = _actionRegistry,
               .runtime = runtime,
               .parentWindow = parentWindow}
    , _host{_registry}
    , _config{std::move(config)}
  {
    layout::LayoutRuntime::registerStandardComponents(_registry);

    auto const refreshActionStates = [this]
    {
      if (_gioBridgeSession)
      {
        _gioBridgeSession->refreshStates();
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
      if (auto* const queueModel = _context.playback.queueModel; queueModel)
      {
        return layout::ActionState{.enabled = queueModel->isActive(), .disabledReason = ""};
      }

      return layout::ActionState{.enabled = false, .disabledReason = ""};
    };

    registerPlaybackActions(registerAction, hasActiveQueue);
    registerShellActions(registerAction);
    registerWorkspaceActions(registerAction, hasActiveQueue);
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
        if (auto* const queueModel = _context.playback.queueModel; queueModel)
        {
          queueModel->next();
        }
      },
      [this](layout::ActionActivationContext const&) -> layout::ActionState
      {
        if (auto* const queueModel = _context.playback.queueModel; queueModel)
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
        if (auto* const queueModel = _context.playback.queueModel; queueModel)
        {
          queueModel->previous();
        }
      },
      [this](layout::ActionActivationContext const&) -> layout::ActionState
      {
        if (auto* const queueModel = _context.playback.queueModel; queueModel)
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
        if (auto* const queueModel = _context.playback.queueModel; queueModel)
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
        if (auto* const queueModel = _context.playback.queueModel; queueModel)
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
                     if (auto const menu = _context.shell.menuModel; menu)
                     {
                       auto* const popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
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
                     if (_config)
                     {
                       openEditor(*_config);
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

  void ShellLayoutController::attachToWindow()
  {
    _context.parentWindow.set_child(_host);

    if (auto* actionMap = dynamic_cast<Gio::ActionMap*>(&_context.parentWindow); actionMap)
    {
      _gioBridgeSession = layout::GioActionBridge::exportActions(_actionRegistry, *actionMap, *this);
    }
    else
    {
      APP_LOG_WARN("ShellLayoutController parentWindow is not a Gio::ActionMap. Skipping action export.");
    }
  }

  void ShellLayoutController::refreshExportedActions()
  {
    if (_gioBridgeSession)
    {
      _gioBridgeSession->refreshStates();
    }
  }

  void ShellLayoutController::loadLayout(AppConfig& config)
  {
    auto& runtime = _context.runtime.async();
    runtime.spawnWithLifetime(
      &_tasks,
      [](ShellLayoutController* self, AppConfig* cfg) -> rt::async::Task<void>
      {
        APP_LOG_DEBUG("ShellLayoutController: loadLayout coroutine started on UI thread");

        auto* const asyncRuntime = &self->_context.runtime.async();
        co_await asyncRuntime->resumeOnWorker();
        APP_LOG_DEBUG("ShellLayoutController: loading config on background worker thread");

        auto prefs = rt::AppPrefsState{};
        cfg->loadAppPrefs(prefs);

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

        auto doc = layout::createBuiltInLayout(presetId);
        auto const customized = cfg->loadShellLayout(doc, presetIdStr);

        co_await asyncRuntime->resumeOnControl();
        APP_LOG_DEBUG("ShellLayoutController: resumed on UI thread, applying layout");

        self->_isCustomized = customized;
        self->_activePresetId = std::move(presetIdStr);
        self->_activeLayout = doc;
        self->_host.setLayout(self->_context, self->_activeLayout);
      }(this, &config));
  }

  void ShellLayoutController::saveLayout(AppConfig& config) const
  {
    if (_isCustomized)
    {
      config.saveShellLayout(_activeLayout, _activePresetId);
    }
  }

  void ShellLayoutController::openEditor(AppConfig& config)
  {
    auto prefs = rt::AppPrefsState{};
    config.loadAppPrefs(prefs);

    auto const initialPresetId = _activePresetId.empty() ? "classic" : _activePresetId;

    auto const dialog = std::make_shared<layout::editor::LayoutEditorDialog>(
      dynamic_cast<Gtk::Window&>(_context.parentWindow), _registry, _actionRegistry, _activeLayout, initialPresetId);
    auto* const dialogPtr = dialog.get();

    _context.editMode = true;
    _context.onNodeMoved = [dialogPtr](std::string const& nodeId, std::int32_t posX, std::int32_t posY)
    { dialogPtr->updateNodePosition(nodeId, posX, posY); };

    _host.setLayout(_context, _activeLayout);

    dialogPtr->signalApplyPreview().connect([this](layout::LayoutDocument const& doc)
                                            { _host.setLayout(_context, doc); });

    dialogPtr->signalSaveRequest().connect(
      [this, sharedDialog = dialog, &config](layout::LayoutDocument const& doc)
      {
        _activeLayout = doc;
        _isCustomized = true;

        if (auto const presetIdDialog = sharedDialog->selectedPresetId(); !presetIdDialog.empty())
        {
          _activePresetId = presetIdDialog;

          auto prefsUpdate = rt::AppPrefsState{};
          config.loadAppPrefs(prefsUpdate);
          prefsUpdate.lastLayoutPreset = _activePresetId;
          config.saveAppPrefs(prefsUpdate);
        }

        _host.setLayout(_context, _activeLayout);
        config.saveShellLayout(_activeLayout, _activePresetId);
      });

    dialogPtr->signal_hide().connect(
      [this, sharedDialog = dialog]
      {
        _context.editMode = false;
        _context.onNodeMoved = nullptr;
      });

    dialogPtr->signal_response().connect(
      [this, sharedDialog = dialog](std::int32_t responseId)
      {
        if (responseId == Gtk::ResponseType::CANCEL)
        {
          _host.setLayout(_context, _activeLayout);
        }
      });

    dialogPtr->present();
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
