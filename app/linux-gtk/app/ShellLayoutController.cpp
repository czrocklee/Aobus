// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "ShellLayoutController.h"

#include "AppConfigStore.h"
#include "ShellLayoutComponentStateStore.h"
#include "ShellLayoutStore.h"
#include "app/GtkUiDependencies.h"
#include "app/ThemeCoordinator.h"
#include "i18n/GtkTextCatalog.h"
#include "layout/document/LayoutDialect.h"
#include "layout/document/LayoutPresets.h"
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
#include "track/TrackOrderActions.h"
#include "track/TrackPageHost.h"
#include "track/TrackViewPage.h"
#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/AppState.h>
#include <ao/rt/Log.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/WorkspaceSnapshot.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryAuthoring.h>
#include <ao/rt/playback/PlaybackService.h>
#include <ao/rt/projection/TrackDetailProjection.h>
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutStatePromoter.h>
#include <ao/uimodel/layout/document/LayoutNodeId.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/layout/document/LayoutValidation.h>
#include <ao/uimodel/layout/shell/LayoutBuildStateView.h>
#include <ao/uimodel/layout/shell/ShellLayoutSessionModel.h>
#include <ao/uimodel/playback/command/PlaybackCommand.h>
#include <ao/uimodel/playback/command/PlaybackCommandSurface.h>
#include <ao/uimodel/preference/ThemePreset.h>
#include <ao/uimodel/presentation/PresentationText.h>

#include <glibmm/main.h>
#include <gtkmm/dialog.h>
#include <gtkmm/enums.h>
#include <gtkmm/popovermenu.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
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
    using i18n::MessageId;

    struct LayoutLoadResult final
    {
      std::string presetId;
      uimodel::LayoutDocument document;
      uimodel::PreparedLayout preparedLayout;
      uimodel::LayoutComponentStateDocument componentState;
    };

    Result<uimodel::PreparedLayout> prepareValidatedLayout(uimodel::LayoutDocument const& document,
                                                           uimodel::LayoutDocumentLimits const& limits,
                                                           uimodel::LayoutComponentCatalog const& components,
                                                           uimodel::LayoutActionCatalog const& actions)
    {
      auto preparedRes = uimodel::prepareLayout(document, limits);

      if (!preparedRes)
      {
        return std::unexpected{preparedRes.error()};
      }

      if (auto validatedRes = uimodel::requireValidLayout(*preparedRes, components, actions, layout::layoutDialect());
          !validatedRes)
      {
        return std::unexpected{validatedRes.error()};
      }

      return preparedRes;
    }

    LayoutLoadResult loadLayoutOnWorker(ShellLayoutStore& store,
                                        ShellLayoutComponentStateStore* componentStateStore,
                                        AppConfigStore& configStore,
                                        uimodel::LayoutComponentCatalog const& components,
                                        uimodel::LayoutActionCatalog const& actions)
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
      auto loadedRes = store.load(selection.presetId);
      bool usingCustomLayout = false;
      auto doc = uimodel::LayoutDocument{};

      if (!loadedRes)
      {
        APP_LOG_WARN("ShellLayoutController: Rejected custom layout for preset '{}': {}",
                     selection.presetId,
                     loadedRes.error().message);
        doc = layout::makeBuiltInLayout(presetId);
      }
      else if (*loadedRes)
      {
        usingCustomLayout = true;
        doc = std::move(**loadedRes);
      }
      else
      {
        doc = layout::makeBuiltInLayout(presetId);
      }

      auto preparedRes = prepareValidatedLayout(doc, store.limits(), components, actions);

      if (!preparedRes && usingCustomLayout)
      {
        APP_LOG_WARN("ShellLayoutController: Rejected custom layout for preset '{}': {}",
                     selection.presetId,
                     preparedRes.error().message);
        doc = layout::makeBuiltInLayout(presetId);
        preparedRes = prepareValidatedLayout(doc, store.limits(), components, actions);
      }

      AO_INVARIANT(preparedRes, "Built-in shell layout is invalid");

      auto stateDoc = componentStateStore == nullptr
                        ? uimodel::ShellLayoutSessionModel::emptyComponentState(selection.presetId)
                        : componentStateStore->load(selection.presetId)
                            .value_or(uimodel::ShellLayoutSessionModel::emptyComponentState(selection.presetId));

      return {.presetId = selection.presetId,
              .document = std::move(doc),
              .preparedLayout = std::move(*preparedRes),
              .componentState = std::move(stateDoc)};
    }

    uimodel::PlaybackCommandSurface& commandSurface(uimodel::PlaybackCommandSurface* surface)
    {
      AO_EXPECTS(surface != nullptr, "ShellLayoutController: playback command surface is not bound");

      return *surface;
    }

    ThemeCoordinator& requireThemeCoordinator(GtkUiDependencies const& dependencies)
    {
      AO_EXPECTS(dependencies.themeCoordinator != nullptr, "ShellLayoutController: theme coordinator is not bound");

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
    , _dependencies{std::move(dependencies)}
    , _host{_registry}
    , _configStorePtr{std::move(configStorePtr)}
    , _layoutStorePtr{std::move(layoutStorePtr)}
    , _componentStateStorePtr{std::move(componentStateStorePtr)}
    , _themeCoordinator{requireThemeCoordinator(_dependencies)}
    , _callbackScope{[this]
                     {
                       _queuedEditorDialogRetirementConnection.disconnect();
                       _queuedSoulWindowRetirementConnection.disconnect();
                       _queuedOpenEditorConnection.disconnect();
                     }}
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

    auto const hasActiveSequence = [this](layout::ActionActivationContext const&) -> layout::ActionAvailability
    {
      return layout::ActionAvailability{
        .enabled = _runtime.playback().snapshot().succession.currentTrackId != kInvalidTrackId, .disabledReason = ""};
    };

    registerPlaybackActions(registerAction);
    registerShellActions(registerAction);
    registerWorkspaceActions(registerAction, hasActiveSequence);
    registerTrackActions(registerAction);

    _actionStateSubscriptions.push_back(
      commandSurface(_dependencies.playbackCommandSurface).onAvailabilityChanged([this] { refreshExportedActions(); }));
    _actionStateSubscriptions.push_back(_runtime.views().onSelectionChanged(
      [this](rt::ViewService::SelectionChanged const&) { refreshExportedActions(); }));
    _actionStateSubscriptions.push_back(_runtime.views().onPresentationChanged(
      [this](rt::ViewService::PresentationChanged const&) { refreshExportedActions(); }));
    _actionStateSubscriptions.push_back(_runtime.views().onProjectionChanged(
      [this](rt::TrackListProjectionChanged const&) { refreshExportedActions(); }));
    _actionStateSubscriptions.push_back(_runtime.views().onFilterErrorChanged(
      [this](rt::ViewService::FilterErrorChanged const&) { refreshExportedActions(); }));
    _actionStateSubscriptions.push_back(
      _runtime.views().onViewDestroyed([this](rt::ViewService::ViewDestroyed const&) { refreshExportedActions(); }));
    _actionStateSubscriptions.push_back(
      _runtime.workspace().onChanged([this](rt::WorkspaceChanged const&) { refreshExportedActions(); }));
    _actionStateSubscriptions.push_back(_runtime.library().onAuthoringAvailabilityChanged(
      [this](rt::LibraryAuthoringAvailability const&) { refreshExportedActions(); }));
  }

  ShellLayoutController::~ShellLayoutController()
  {
    _callbackScope.close();
    _queuedEditorDialogRetirementConnection.disconnect();
    _queuedSoulWindowRetirementConnection.disconnect();
    _soulWindowHideConnection.disconnect();
    _soulWindowRetirementQueued = false;
    _soulWindowPtr.reset();
    _tasks.cancelAll();
    _optEditorThemeToken.reset();
    _editorDialogPtr.reset();
    _outputDevicePopover.detach();
    _menuPopover.detach();
    // Components retain LayoutRuntimeState and may flush pending state while
    // destructing, so release them before the state and its store owner.
    _host.clearLayout();
  }

  Gtk::Window* ShellLayoutController::soulWindow() const noexcept
  {
    return _soulWindowPtr.get();
  }

  void ShellLayoutController::setMenuModel(Glib::RefPtr<Gio::MenuModel> menuModelPtr)
  {
    _dependencies.menuModelPtr = std::move(menuModelPtr);
  }

  void ShellLayoutController::registerPlaybackActions(RegisterActionFn const& registerAction)
  {
    auto const execute = [this](uimodel::PlaybackCommand command)
    {
      return [this, command](layout::ActionActivationContext&)
      { commandSurface(_dependencies.playbackCommandSurface).execute(command); };
    };

    auto const isEnabled = [this](uimodel::PlaybackCommand command)
    {
      return [this, command](layout::ActionActivationContext const&) -> layout::ActionAvailability
      {
        return layout::ActionAvailability{
          .enabled = commandSurface(_dependencies.playbackCommandSurface).isEnabled(command), .disabledReason = ""};
      };
    };

    for (auto const command : uimodel::playbackCommands())
    {
      registerAction(uimodel::playbackCommandActionId(command),
                     uimodel::playbackActionLabel(_dependencies.textCatalog, command),
                     gtkText(_dependencies.textCatalog, MessageId::GtkActionCategoryPlayback),
                     uimodel::LayoutActionCapability::None,
                     execute(command),
                     isEnabled(command));
    }

    registerAction("playback.showOutputDeviceSelector",
                   gtkText(_dependencies.textCatalog, MessageId::GtkActionOutputDevices),
                   gtkText(_dependencies.textCatalog, MessageId::GtkActionCategoryPlayback),
                   uimodel::LayoutActionCapability::RequiresAnchor | uimodel::LayoutActionCapability::PresentsMenu,
                   [this](layout::ActionActivationContext& ctx)
                   {
                     if (_outputDevicePopover.hasPopover())
                     {
                       return;
                     }

                     auto popoverPtr = std::make_unique<OutputDevicePopover>(ctx.runtime.playback(),
                                                                             _dependencies.textCatalog,
                                                                             _dependencies.outputDeviceIntent,
                                                                             Gtk::PositionType::BOTTOM);
                     _outputDevicePopover.attach(std::move(popoverPtr), ctx.anchorWidget);
                     _outputDevicePopover.popup();
                   },
                   {});
  }

  void ShellLayoutController::registerShellActions(RegisterActionFn const& registerAction)
  {
    registerAction("shell.showSystemMenu",
                   gtkText(_dependencies.textCatalog, MessageId::GtkActionSystemMenu),
                   gtkText(_dependencies.textCatalog, MessageId::GtkActionCategoryShell),
                   uimodel::LayoutActionCapability::RequiresAnchor | uimodel::LayoutActionCapability::PresentsMenu,
                   [this](layout::ActionActivationContext& ctx)
                   {
                     if (_dependencies.menuModelPtr)
                     {
                       if (_menuPopover.hasPopover())
                       {
                         return;
                       }

                       auto popoverPtr = std::make_unique<Gtk::PopoverMenu>(_dependencies.menuModelPtr);
                       popoverPtr->set_has_arrow(true);
                       _menuPopover.attach(std::move(popoverPtr), ctx.anchorWidget);
                       _menuPopover.popup();
                     }
                     else
                     {
                       APP_LOG_WARN("shell.showSystemMenu invoked but menuModel is missing");
                     }
                   },
                   {});

    registerAction("shell.showSoul",
                   "Aobus Soul",
                   gtkText(_dependencies.textCatalog, MessageId::GtkActionCategoryShell),
                   uimodel::LayoutActionCapability::None,
                   [this](layout::ActionActivationContext& ctx) { presentSoul(ctx); },
                   {});

    registerAction("shell.editLayout",
                   gtkText(_dependencies.textCatalog, MessageId::GtkShellEditLayout),
                   gtkText(_dependencies.textCatalog, MessageId::GtkActionCategoryShell),
                   uimodel::LayoutActionCapability::None,
                   [this](layout::ActionActivationContext&)
                   {
                     _queuedOpenEditorConnection.disconnect();
                     auto openEditorAfterDispatch = _callbackScope.guard(
                       [this]
                       {
                         if (_configStorePtr)
                         {
                           openEditor(*_configStorePtr);
                         }
                       });
                     _queuedOpenEditorConnection = Glib::signal_idle().connect(
                       [openEditorAfterDispatch = std::move(openEditorAfterDispatch)] mutable
                       {
                         openEditorAfterDispatch();
                         return false;
                       });
                   },
                   {});
  }

  void ShellLayoutController::presentSoul(layout::ActionActivationContext& context)
  {
    if (_soulWindowPtr)
    {
      _queuedSoulWindowRetirementConnection.disconnect();
      _soulWindowRetirementQueued = false;
      _soulWindowPtr->present();
      return;
    }

    _soulWindowPtr = std::make_unique<AobusSoulWindow>();
    auto* const window = _soulWindowPtr.get();
    window->set_transient_for(context.parentWindow);
    window->bind(context.runtime.playback());
    _soulWindowHideConnection = window->signal_hide().connect(
      [this, window]
      {
        if (_soulWindowRetirementQueued)
        {
          return;
        }

        _soulWindowRetirementQueued = true;
        auto retireWindow = _callbackScope.guard(
          [this, window]
          {
            _soulWindowRetirementQueued = false;

            if (_soulWindowPtr.get() != window)
            {
              return;
            }

            _soulWindowHideConnection.disconnect();
            _soulWindowPtr.reset();
          });
        _queuedSoulWindowRetirementConnection = Glib::signal_idle().connect(
          [retireWindow = std::move(retireWindow)] mutable
          {
            retireWindow();
            return false;
          });
      });
    window->present();
  }

  void ShellLayoutController::registerWorkspaceActions(RegisterActionFn const& registerAction,
                                                       layout::ActionStateProvider const& hasActiveSequence)
  {
    registerAction(
      "workspace.revealCurrentTrack",
      gtkText(_dependencies.textCatalog, MessageId::GtkActionRevealTrack),
      gtkText(_dependencies.textCatalog, MessageId::GtkActionCategoryWorkspace),
      uimodel::LayoutActionCapability::None,
      [](layout::ActionActivationContext& ctx) { ctx.runtime.playback().commands().revealPlayingTrack(); },
      hasActiveSequence);
  }

  void ShellLayoutController::registerTrackActions(RegisterActionFn const& registerAction)
  {
    registerAction(
      "track.presentProperties",
      gtkText(_dependencies.textCatalog, MessageId::GtkActionTrackProperties),
      gtkText(_dependencies.textCatalog, MessageId::GtkActionCategoryTracks),
      uimodel::LayoutActionCapability::None,
      [this](layout::ActionActivationContext& ctx)
      {
        if (_dependencies.tagEditController != nullptr)
        {
          auto const target = rt::FocusedViewTarget{};
          auto projPtr = ctx.runtime.workspace().detailProjection(target);

          if (auto const snap = projPtr->snapshot(); !snap.trackIds.empty())
          {
            _dependencies.tagEditController->presentProperties(
              TrackSelection{.listId = kInvalidListId, .selectedIds = snap.trackIds});
          }
        }
      },
      [](layout::ActionActivationContext const& ctx) -> layout::ActionAvailability
      {
        auto const target = rt::FocusedViewTarget{};
        auto projPtr = ctx.runtime.workspace().detailProjection(target);
        return layout::ActionAvailability{.enabled = !projPtr->snapshot().trackIds.empty(), .disabledReason = ""};
      });

    registerAction(
      "track.editTags",
      gtkText(_dependencies.textCatalog, MessageId::GtkActionEditTags),
      gtkText(_dependencies.textCatalog, MessageId::GtkActionCategoryTracks),
      uimodel::LayoutActionCapability::RequiresAnchor | uimodel::LayoutActionCapability::PresentsMenu,
      [this](layout::ActionActivationContext& ctx)
      {
        if (_dependencies.tagEditController != nullptr)
        {
          auto const target = rt::FocusedViewTarget{};
          auto projPtr = ctx.runtime.workspace().detailProjection(target);

          if (auto const snap = projPtr->snapshot(); !snap.trackIds.empty())
          {
            _dependencies.tagEditController->openTagEditor(
              TrackSelection{.listId = kInvalidListId, .selectedIds = snap.trackIds}, ctx.anchorWidget);
          }
        }
      },
      [](layout::ActionActivationContext const& ctx) -> layout::ActionAvailability
      {
        auto const target = rt::FocusedViewTarget{};
        auto projPtr = ctx.runtime.workspace().detailProjection(target);
        return layout::ActionAvailability{.enabled = !projPtr->snapshot().trackIds.empty(), .disabledReason = ""};
      });

    registerTrackOrderActions(registerAction);
  }

  void ShellLayoutController::registerTrackOrderActions(RegisterActionFn const& registerAction)
  {
    auto const registerOrderAction =
      [this, &registerAction](std::string_view const id, std::string_view const label, TrackOrderCommand const command)
    {
      registerAction(
        id,
        label,
        gtkText(_dependencies.textCatalog, MessageId::GtkActionCategoryTracks),
        uimodel::LayoutActionCapability::None,
        [this, command](layout::ActionActivationContext&)
        {
          if (_dependencies.trackPageHost == nullptr)
          {
            return;
          }

          auto* const entry = _dependencies.trackPageHost->currentVisible();

          if (entry != nullptr && entry->pagePtr != nullptr)
          {
            entry->pagePtr->applyListOrderCommand(command);
          }
        },
        [this, command](layout::ActionActivationContext const&) -> layout::ActionAvailability
        {
          if (_dependencies.trackPageHost == nullptr)
          {
            return {.enabled = false,
                    .disabledReason = gtkText(_dependencies.textCatalog, i18n::MessageId::GtkNoTrackViewAvailable)};
          }

          auto const* const entry = _dependencies.trackPageHost->currentVisible();

          if (entry == nullptr || entry->pagePtr == nullptr)
          {
            return {.enabled = false,
                    .disabledReason = gtkText(_dependencies.textCatalog, i18n::MessageId::GtkNoTrackViewAvailable)};
          }

          auto const capabilities = entry->pagePtr->orderCapabilities();
          bool enabled = false;

          switch (command)
          {
            case TrackOrderCommand::MoveUp:
            case TrackOrderCommand::MoveDown: enabled = capabilities.canRelativeMove; break;
            case TrackOrderCommand::MoveToTop:
            case TrackOrderCommand::MoveToBottom: enabled = capabilities.canAbsoluteMove; break;
            case TrackOrderCommand::Reset: enabled = capabilities.canResetOrder; break;
            case TrackOrderCommand::ForgetHidden: enabled = capabilities.canForgetHiddenPositions; break;
          }

          if (enabled && command != TrackOrderCommand::Reset && command != TrackOrderCommand::ForgetHidden)
          {
            enabled = entry->pagePtr->selectionController().selectedTrackCount() > 0;
          }

          auto disabledReason = std::string{};

          if (!enabled)
          {
            disabledReason = capabilities.disabledReason.empty()
                               ? gtkText(_dependencies.textCatalog, i18n::MessageId::GtkListSelectTracksForOrder)
                               : capabilities.disabledReason;
          }

          return {.enabled = enabled, .disabledReason = std::move(disabledReason)};
        });
    };

    registerOrderAction(kTrackOrderMoveUpActionId,
                        gtkText(_dependencies.textCatalog, MessageId::GtkListMoveUpAction),
                        TrackOrderCommand::MoveUp);
    registerOrderAction(kTrackOrderMoveDownActionId,
                        gtkText(_dependencies.textCatalog, MessageId::GtkListMoveDownAction),
                        TrackOrderCommand::MoveDown);
    registerOrderAction(kTrackOrderMoveToTopActionId,
                        gtkText(_dependencies.textCatalog, MessageId::GtkListMoveToTopAction),
                        TrackOrderCommand::MoveToTop);
    registerOrderAction(kTrackOrderMoveToBottomActionId,
                        gtkText(_dependencies.textCatalog, MessageId::GtkListMoveToBottomAction),
                        TrackOrderCommand::MoveToBottom);
    registerOrderAction(kTrackOrderResetActionId,
                        gtkText(_dependencies.textCatalog, MessageId::GtkListResetOrderAction),
                        TrackOrderCommand::Reset);
    registerOrderAction(kTrackOrderForgetHiddenActionId,
                        gtkText(_dependencies.textCatalog, MessageId::GtkListForgetHiddenPositions),
                        TrackOrderCommand::ForgetHidden);
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

  Result<layout::LayoutHost::PreparedTree> ShellLayoutController::prepareHost(
    uimodel::PreparedLayout const& preparedLayout,
    uimodel::LayoutBuildStateView buildState)
  {
    auto ctx = layout::LayoutBuildContext{.registry = _registry,
                                          .actionRegistry = _actionRegistry,
                                          .runtime = _runtime,
                                          .parentWindow = _parentWindow,
                                          .runtimeState = _runtimeState,
                                          .buildState = std::move(buildState),
                                          .dependencies = _dependencies};
    return _host.prepare(ctx, preparedLayout);
  }

  uimodel::LayoutDocumentLimits const& ShellLayoutController::layoutLimits() const noexcept
  {
    static auto const kDefaultLimits = uimodel::LayoutDocumentLimits{};
    return _layoutStorePtr ? _layoutStorePtr->limits() : kDefaultLimits;
  }

  void ShellLayoutController::rebuildHost(uimodel::LayoutDocument const& doc)
  {
    rebuildHost(doc, uimodel::LayoutBuildStateView{_runtimeState});
  }

  void ShellLayoutController::rebuildHost(uimodel::LayoutDocument const& doc, uimodel::LayoutBuildStateView buildState)
  {
    auto preparedRes = prepareValidatedLayout(doc, layoutLimits(), _registry.catalog(), _actionRegistry.catalog());

    if (!preparedRes)
    {
      APP_LOG_WARN("ShellLayoutController: Rejected layout rebuild: {}", preparedRes.error().message);
      return;
    }

    auto pendingRes = prepareHost(*preparedRes, std::move(buildState));

    if (!pendingRes)
    {
      APP_LOG_ERROR("ShellLayoutController: Failed to prepare layout rebuild: {}", pendingRes.error().message);
      return;
    }

    _host.commit(_runtimeState, std::move(*pendingRes));
  }

  void ShellLayoutController::loadLayout()
  {
    auto* const asyncRuntime = &_runtime.async();
    // startCancellable invokes the task factory on a worker. Snapshot all
    // controller-owned inputs before publishing the lazy coroutine.
    auto componentCatalog = _registry.catalog();
    auto actionCatalog = _actionRegistry.catalog();

    asyncRuntime->spawnWithLifetime(
      _tasks,
      [controller = this,
       asyncRuntime,
       storePtr = _layoutStorePtr,
       componentStateStorePtr = _componentStateStorePtr,
       configStorePtr = _configStorePtr,
       componentCatalog = std::move(componentCatalog),
       actionCatalog = std::move(actionCatalog)](std::stop_token const stopToken) mutable
      {
        return loadLayoutWorkflow(controller,
                                  asyncRuntime,
                                  std::move(storePtr),
                                  std::move(componentStateStorePtr),
                                  std::move(configStorePtr),
                                  std::move(componentCatalog),
                                  std::move(actionCatalog),
                                  stopToken);
      },
      "shell layout load workflow");
  }

  async::Task<void> ShellLayoutController::loadLayoutWorkflow(
    ShellLayoutController* const controller,
    async::Runtime* const asyncRuntime,
    std::shared_ptr<ShellLayoutStore> layoutStorePtr,
    std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
    std::shared_ptr<AppConfigStore> configStorePtr,
    uimodel::LayoutComponentCatalog componentCatalog,
    uimodel::LayoutActionCatalog actionCatalog,
    std::stop_token const stopToken)
  {
    APP_LOG_DEBUG("ShellLayoutController: loadLayout coroutine started");

    auto optRes = std::optional<LayoutLoadResult>{};
    co_await asyncRuntime->resumeOnWorker(stopToken);
    APP_LOG_DEBUG("ShellLayoutController: loading layout config on background worker thread");

    if (layoutStorePtr && configStorePtr)
    {
      optRes = loadLayoutOnWorker(
        *layoutStorePtr, componentStateStorePtr.get(), *configStorePtr, componentCatalog, actionCatalog);
    }

    co_await asyncRuntime->resumeOnCallbackExecutor(stopToken);

    if (!optRes)
    {
      co_return;
    }

    APP_LOG_DEBUG("ShellLayoutController: resumed on UI thread, applying layout");
    controller->applyLoadedLayout(std::move(optRes->presetId),
                                  std::move(optRes->document),
                                  std::move(optRes->preparedLayout),
                                  std::move(optRes->componentState));
  }

  void ShellLayoutController::applyLoadedLayout(std::string presetId,
                                                uimodel::LayoutDocument document,
                                                uimodel::PreparedLayout preparedLayout,
                                                uimodel::LayoutComponentStateDocument componentState)
  {
    auto pendingRes = prepareHost(
      preparedLayout, uimodel::LayoutBuildStateView{presetId, componentState, _runtimeState.componentStateGeneration});

    if (!pendingRes)
    {
      APP_LOG_ERROR(
        "ShellLayoutController: Failed to prepare loaded layout '{}': {}", presetId, pendingRes.error().message);
      return;
    }

    for (auto const& diagnostic : uimodel::validateStatefulLayoutNodeIds(preparedLayout, _registry.catalog()))
    {
      if (diagnostic.severity == uimodel::LayoutNodeIdDiagnosticSeverity::Error)
      {
        APP_LOG_ERROR("ShellLayoutController: Layout id error in preset '{}' component '{}' ({}): {}",
                      presetId,
                      diagnostic.componentId,
                      diagnostic.componentType,
                      diagnostic.message);
      }
      else
      {
        APP_LOG_WARN("ShellLayoutController: Layout id warning in preset '{}' component '{}' ({}): {}",
                     presetId,
                     diagnostic.componentId,
                     diagnostic.componentType,
                     diagnostic.message);
      }
    }

    // Invalidate the old generation before replacing the shared component-state document.
    _runtimeState.componentStateGeneration = pendingRes->componentStateGeneration();
    _session.applyLayout(std::move(presetId), std::move(document));
    auto const snapshot = _session.snapshot();
    _runtimeState.activePresetId = snapshot.presetId;
    _runtimeState.componentState = std::move(componentState);
    _host.commit(_runtimeState, std::move(*pendingRes));
  }

  void ShellLayoutController::openEditor(AppConfigStore& configStore)
  {
    auto prefs = rt::AppPrefsState{};
    configStore.loadAppPrefs(prefs);

    auto const initialPresetId =
      uimodel::ShellLayoutSessionModel::activeOrDefaultPresetId(_session.snapshot().presetId);
    auto const initialThemeId = std::string{uimodel::themePresetId(_themeCoordinator.activeTheme())};

    auto loader = [storePtr = _layoutStorePtr](std::string_view id) -> uimodel::LayoutDocument
    {
      if (storePtr)
      {
        auto loadedRes = storePtr->load(id);

        if (loadedRes && *loadedRes)
        {
          return std::move(**loadedRes);
        }

        if (!loadedRes)
        {
          APP_LOG_WARN(
            "ShellLayoutController: Editor rejected custom layout for preset '{}': {}", id, loadedRes.error().message);
        }
      }

      return layout::makeBuiltInLayout(layout::presetIdFromString(id));
    };

    _editorDialogPtr = std::make_shared<layout::editor::LayoutEditorDialog>(dynamic_cast<Gtk::Window&>(_parentWindow),
                                                                            _registry,
                                                                            _actionRegistry,
                                                                            _dependencies.textCatalog,
                                                                            _session.snapshot().layout,
                                                                            initialPresetId,
                                                                            initialThemeId,
                                                                            std::move(loader),
                                                                            layout::editor::PreviewSchedulerFn{},
                                                                            layoutLimits());
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
                                            { _themeCoordinator.setTheme(uimodel::themePresetFromId(themeId)); });

    dialogRaw->signalSaveRequest().connect([this](layout::editor::LayoutSaveResult const& result)
                                           { return this->handleEditorSaveRequested(result); });

    dialogRaw->signal_hide().connect(
      [this, dialogRaw]
      {
        if (_editorDialogPtr.get() != dialogRaw)
        {
          return;
        }

        _runtimeState.editMode = false;
        _runtimeState.onNodeMoved = nullptr;
        _optEditorThemeToken.reset();
        auto hiddenDialogPtr = std::exchange(_editorDialogPtr, {});
        auto retireDialog =
          _callbackScope.guard([hiddenDialogPtr = std::move(hiddenDialogPtr)] mutable { hiddenDialogPtr.reset(); });
        _queuedEditorDialogRetirementConnection.disconnect();
        _queuedEditorDialogRetirementConnection = Glib::signal_idle().connect(
          [retireDialog = std::move(retireDialog)] mutable
          {
            retireDialog();
            return false;
          });
      });

    dialogRaw->signal_response().connect(
      [this, oldTheme = _themeCoordinator.activeTheme()](std::int32_t responseId)
      {
        if (responseId == Gtk::ResponseType::CANCEL)
        {
          auto const snapshot = _session.snapshot();
          rebuildHost(snapshot.layout,
                      uimodel::LayoutBuildStateView{
                        snapshot.presetId, _runtimeState.componentState, _runtimeState.componentStateGeneration});
          _themeCoordinator.setTheme(oldTheme);
        }
      });

    dialogRaw->present();
  }

  Result<> ShellLayoutController::handleEditorSaveRequested(layout::editor::LayoutSaveResult const& result)
  {
    auto preparedModified = std::map<std::string, uimodel::PreparedLayout, std::less<>>{};

    for (auto const& [id, doc] : result.modified)
    {
      auto preparedRes = prepareValidatedLayout(doc, layoutLimits(), _registry.catalog(), _actionRegistry.catalog());

      if (!preparedRes)
      {
        APP_LOG_WARN(
          "ShellLayoutController: Rejected editor save for preset '{}': {}", id, preparedRes.error().message);
        return std::unexpected{preparedRes.error()};
      }

      preparedModified.emplace(id, std::move(*preparedRes));
    }

    auto activePreparedRes =
      prepareValidatedLayout(result.activeDocument, layoutLimits(), _registry.catalog(), _actionRegistry.catalog());

    if (!activePreparedRes)
    {
      APP_LOG_WARN("ShellLayoutController: Rejected active editor layout '{}': {}",
                   result.activePresetId,
                   activePreparedRes.error().message);
      return std::unexpected{activePreparedRes.error()};
    }

    auto nextComponentState = uimodel::ShellLayoutSessionModel::emptyComponentState(result.activePresetId);
    auto const activeReset = std::ranges::contains(result.resets, result.activePresetId);

    if (_componentStateStorePtr && !activeReset)
    {
      nextComponentState = _componentStateStorePtr->load(result.activePresetId)
                             .value_or(uimodel::ShellLayoutSessionModel::emptyComponentState(result.activePresetId));
      uimodel::pruneComponentState(nextComponentState, *activePreparedRes, _registry.catalog());
    }

    auto pendingRes = prepareHost(
      *activePreparedRes,
      uimodel::LayoutBuildStateView{result.activePresetId, nextComponentState, _runtimeState.componentStateGeneration});

    if (!pendingRes)
    {
      APP_LOG_ERROR("ShellLayoutController: Failed to prepare editor save for preset '{}': {}",
                    result.activePresetId,
                    pendingRes.error().message);
      return std::unexpected{pendingRes.error()};
    }

    if (_layoutStorePtr)
    {
      for (auto const& [id, doc] : result.modified)
      {
        if (auto savedRes = _layoutStorePtr->save(doc, id); !savedRes)
        {
          APP_LOG_ERROR("ShellLayoutController: Failed to save layout preset '{}': {}", id, savedRes.error().message);
          return std::unexpected{savedRes.error()};
        }
      }

      for (auto const& id : result.resets)
      {
        if (auto removedRes = _layoutStorePtr->remove(id); !removedRes)
        {
          APP_LOG_ERROR(
            "ShellLayoutController: Failed to reset layout preset '{}': {}", id, removedRes.error().message);
          return std::unexpected{removedRes.error()};
        }
      }
    }

    if (_componentStateStorePtr)
    {
      for (auto const& item : result.modified)
      {
        if (auto const& id = item.first;
            !_componentStateStorePtr->prune(id, preparedModified.at(id), _registry.catalog()))
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

    if (_configStorePtr)
    {
      auto prefsUpdate = rt::AppPrefsState{};
      _configStorePtr->loadAppPrefs(prefsUpdate);
      prefsUpdate.lastLayoutPreset = result.activePresetId;
      _configStorePtr->saveAppPrefs(prefsUpdate);
      _themeCoordinator.setTheme(uimodel::themePresetFromId(prefsUpdate.lastThemePreset));
    }

    // Invalidate the retiring generation before replacing its shared state document.
    _runtimeState.componentStateGeneration = pendingRes->componentStateGeneration();
    _session.applyLayout(result.activePresetId, result.activeDocument);
    auto const snapshot = _session.snapshot();
    _runtimeState.activePresetId = snapshot.presetId;
    _runtimeState.componentState = std::move(nextComponentState);
    _host.commit(_runtimeState, std::move(*pendingRes));
    return {};
  }

  void ShellLayoutController::resetRuntimeLayoutState()
  {
    auto const presetId = uimodel::ShellLayoutSessionModel::activeOrDefaultPresetId(_session.snapshot().presetId);
    auto nextComponentState = uimodel::ShellLayoutSessionModel::emptyComponentState(presetId);
    auto preparedRes = prepareValidatedLayout(
      _session.snapshot().layout, layoutLimits(), _registry.catalog(), _actionRegistry.catalog());

    if (!preparedRes)
    {
      APP_LOG_WARN(
        "ShellLayoutController: Rejected runtime-state reset layout '{}': {}", presetId, preparedRes.error().message);
      return;
    }

    auto pendingRes =
      prepareHost(*preparedRes,
                  uimodel::LayoutBuildStateView{presetId, nextComponentState, _runtimeState.componentStateGeneration});

    if (!pendingRes)
    {
      APP_LOG_ERROR(
        "ShellLayoutController: Failed to prepare runtime-state reset '{}': {}", presetId, pendingRes.error().message);
      return;
    }

    if (_componentStateStorePtr)
    {
      if (!_componentStateStorePtr->removePreset(presetId))
      {
        APP_LOG_WARN("ShellLayoutController: Failed to remove runtime state for preset '{}'", presetId);
      }
    }

    _runtimeState.componentStateGeneration = pendingRes->componentStateGeneration();
    auto reset = _session.resetRuntimeLayoutState();
    _runtimeState.activePresetId = reset.presetId;
    _runtimeState.componentState = std::move(reset.componentState);
    _host.commit(_runtimeState, std::move(*pendingRes));
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

    auto presetId = optPromotion->componentState.preset;
    auto apply = [this, presetId, promotion = std::move(*optPromotion)](bool confirmed) mutable
    {
      if (!confirmed)
      {
        APP_LOG_INFO("ShellLayoutController: User cancelled promoting panel sizes for preset '{}'", presetId);
        return;
      }

      applyPromotedPanelSizes(std::move(promotion.layout), std::move(promotion.componentState));
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

  void ShellLayoutController::applyPromotedPanelSizes(uimodel::LayoutDocument promotedLayout,
                                                      uimodel::LayoutComponentStateDocument promotedState)
  {
    auto const& presetId = promotedState.preset;
    auto preparedRes =
      prepareValidatedLayout(promotedLayout, layoutLimits(), _registry.catalog(), _actionRegistry.catalog());

    if (!preparedRes)
    {
      APP_LOG_WARN("ShellLayoutController: Rejected promoted layout '{}': {}", presetId, preparedRes.error().message);
      return;
    }

    auto pendingRes = prepareHost(
      *preparedRes, uimodel::LayoutBuildStateView{presetId, promotedState, _runtimeState.componentStateGeneration});

    if (!pendingRes)
    {
      APP_LOG_ERROR(
        "ShellLayoutController: Failed to prepare promoted layout '{}': {}", presetId, pendingRes.error().message);
      return;
    }

    if (_layoutStorePtr)
    {
      if (auto savedRes = _layoutStorePtr->save(promotedLayout, presetId); !savedRes)
      {
        APP_LOG_ERROR(
          "ShellLayoutController: Failed to save promoted layout '{}': {}", presetId, savedRes.error().message);
        return;
      }

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

    _session.applyLayout(presetId, std::move(promotedLayout));
    auto const snapshot = _session.snapshot();
    _runtimeState.componentStateGeneration = pendingRes->componentStateGeneration();
    _runtimeState.activePresetId = snapshot.presetId;
    _runtimeState.componentState = std::move(promotedState);
    _host.commit(_runtimeState, std::move(*pendingRes));
    refreshExportedActions();
  }

  void ShellLayoutController::setConfirmPromotionCallback(ConfirmPromotionFn fn)
  {
    _confirmPromotionFn = std::move(fn);
  }

  void ShellLayoutController::activateAction(std::string_view id)
  {
    auto ctx = actionContext(id);

    if (auto const availability = _actionRegistry.state(id, ctx); !availability.enabled)
    {
      if (isTrackOrderAction(id) && _dependencies.trackPageHost != nullptr)
      {
        if (auto* const entry = _dependencies.trackPageHost->currentVisible();
            entry != nullptr && entry->pagePtr != nullptr)
        {
          entry->pagePtr->setStatusMessage(availability.disabledReason);
        }
      }

      return;
    }

    _actionRegistry.activate(id, ctx);
  }

  layout::ActionAvailability ShellLayoutController::actionAvailability(std::string_view id)
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
