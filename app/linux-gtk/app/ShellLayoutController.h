// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/ThemeCoordinator.h"
#include "layout/editor/LayoutEditorDialog.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/GioActionBridge.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutHost.h"
#include "layout/runtime/LayoutRuntimeState.h"
#include <ao/CoreIds.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Task.h>
#include <ao/rt/Subscription.h>
#include <ao/uimodel/layout/action/LayoutActionActivation.h>
#include <ao/uimodel/layout/action/LayoutActionAvailability.h>
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>
#include <ao/uimodel/layout/component/LayoutStatePromoter.h>
#include <ao/uimodel/layout/shell/ShellLayoutSessionModel.h>

#include <gtkmm/window.h>

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
  class PlaybackSequenceService;
}

namespace ao::uimodel
{
  class ListPresentationPreferenceStore;
  class PlaybackCommandSurface;
  class TrackPresentationCatalog;
}

namespace ao::gtk
{
  class AppConfigStore;
  class ImageCache;
  class ListNavigationController;
  struct GtkUiDependencies;
  class ShellLayoutComponentStateStore;
  class ShellLayoutStore;
  class TagEditController;
  class ThemeCoordinator;
  class TrackPageHost;
  class TrackRowCache;
  namespace layout::editor
  {
    class LayoutEditorDialog;
  }
  namespace portal
  {
    class ImportExportActions;
  }

  class ShellLayoutController final : public layout::ActionContextProvider
  {
  public:
    using RegisterActionFn = std::function<void(std::string_view,
                                                std::string_view,
                                                std::string_view,
                                                uimodel::LayoutActionCapabilities,
                                                layout::ActionHandler,
                                                layout::ActionStateProvider)>;

    ShellLayoutController(rt::AppRuntime& runtime,
                          Gtk::Window& parentWindow,
                          std::shared_ptr<AppConfigStore> configStorePtr,
                          std::shared_ptr<ShellLayoutStore> layoutStorePtr,
                          std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
                          GtkUiDependencies dependencies);
    ~ShellLayoutController() override;

    ShellLayoutController(ShellLayoutController const&) = delete;
    ShellLayoutController& operator=(ShellLayoutController const&) = delete;
    ShellLayoutController(ShellLayoutController&&) = delete;
    ShellLayoutController& operator=(ShellLayoutController&&) = delete;

    layout::ComponentRegistry& registry() { return _registry; }
    uimodel::LayoutActionCatalog const& actionCatalog() const { return _actionRegistry.catalog(); }
    layout::LayoutRuntimeState const& runtimeState() const { return _runtimeState; }
    layout::LayoutHost& host() { return _host; }
    uimodel::LayoutDocument const& activeLayout() const { return _session.snapshot().layout; }

    void attachToWindow();
    void setMenuModel(Glib::RefPtr<Gio::MenuModel> menuModelPtr);
    void refreshExportedActions();
    void loadLayout(AppConfigStore& configStore);
    void openEditor(AppConfigStore& configStore);
    void resetRuntimeLayoutState();
    void saveCurrentPanelSizesAsLayoutDefaults();

    using ConfirmPromotionAnswer = std::function<void(bool confirmed)>;
    using ConfirmPromotionFn = std::function<void(std::string const& presetId, ConfirmPromotionAnswer answer)>;
    void setConfirmPromotionCallback(ConfirmPromotionFn fn);

    uimodel::LayoutActionActivationResult activateAction(std::string_view id);
    uimodel::LayoutActionAvailability actionAvailability(std::string_view id);

    layout::editor::LayoutEditorDialog* editorDialog() const { return _editorDialogPtr.get(); }

    layout::ActionActivationContext actionContext(std::string_view componentId) override;
    bool canProvideSafeAnchor(uimodel::LayoutActionDescriptor const& desc) const override;

  private:
    void registerPlaybackActions(RegisterActionFn const& registerAction);
    void registerShellActions(RegisterActionFn const& registerAction);
    void registerWorkspaceActions(RegisterActionFn const& registerAction,
                                  layout::ActionStateProvider const& hasActiveSequence);
    void registerTrackActions(RegisterActionFn const& registerAction);

    void applyPromotedPanelSizes(std::string const& presetId,
                                 uimodel::LayoutDocument promotedLayout,
                                 uimodel::LayoutComponentStateDocument promotedState);

    void applyLoadedLayout(std::string presetId,
                           uimodel::LayoutDocument document,
                           uimodel::LayoutComponentStateDocument componentState);
    async::Task<void> loadLayoutWorkflow(std::shared_ptr<ShellLayoutStore> layoutStorePtr,
                                         std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
                                         std::shared_ptr<AppConfigStore> configStorePtr,
                                         std::stop_token stopToken);
    void applyLoadedLayoutWithFaultReporting(std::string presetId,
                                             uimodel::LayoutDocument document,
                                             uimodel::LayoutComponentStateDocument componentState);

    void handleEditorSaveRequested(layout::editor::LayoutSaveResult const& result);

    /// Assembles a fresh per-build carrier from the owned pieces and (re)builds the host.
    void rebuildHost(uimodel::LayoutDocument const& doc);

    rt::AppRuntime& _runtime;
    Gtk::Window& _parentWindow;
    layout::ComponentRegistry _registry;
    layout::ActionRegistry _actionRegistry;
    layout::LayoutRuntimeState _runtimeState;
    TrackRowCache* _trackRowCache;
    ImageCache* _imageCache;
    rt::PlaybackSequenceService* _playbackSequence;
    uimodel::PlaybackCommandSurface* _playbackCommandSurface;
    TagEditController* _tagEditController;
    portal::ImportExportActions* _importExportActions;
    TrackPageHost* _trackPageHost;
    uimodel::TrackPresentationCatalog* _trackPresentationCatalog;
    uimodel::ListPresentationPreferenceStore* _trackPresentationPreferences;
    ListNavigationController* _listNavigationController;
    std::function<void(ListId, std::string)> _createSmartListFromExpression;
    Glib::RefPtr<Gio::MenuModel> _menuModelPtr;
    layout::LayoutHost _host;
    std::unique_ptr<layout::GioActionBridgeSession> _gioBridgeSessionPtr;
    std::vector<rt::Subscription> _playbackSubs;
    uimodel::ShellLayoutSessionModel _session;
    std::shared_ptr<AppConfigStore> _configStorePtr;
    std::shared_ptr<ShellLayoutStore> _layoutStorePtr;
    std::shared_ptr<ShellLayoutComponentStateStore> _componentStateStorePtr;
    ThemeCoordinator& _themeCoordinator;
    std::optional<ThemeRegistrationToken> _optEditorThemeToken;
    std::shared_ptr<layout::editor::LayoutEditorDialog> _editorDialogPtr;
    async::LifetimeScope _tasks;
    ConfirmPromotionFn _confirmPromotionFn;
  };
} // namespace ao::gtk
