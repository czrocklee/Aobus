// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/ThemeCoordinator.h"
#include "common/MainContextCallbackScope.h"
#include "common/PopoverAttachment.h"
#include "layout/editor/LayoutEditorDialog.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/GioActionBridge.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutHost.h"
#include "layout/runtime/LayoutRuntimeState.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/uimodel/layout/action/LayoutActionAvailability.h>
#include <ao/uimodel/layout/action/LayoutActionCapabilities.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionDescriptor.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/layout/shell/ShellLayoutSessionModel.h>

#include <gtkmm/window.h>
#include <sigc++/scoped_connection.h>

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
    void loadLayout();
    void openEditor(AppConfigStore& configStore);
    void resetRuntimeLayoutState();
    void saveCurrentPanelSizesAsLayoutDefaults();

    using ConfirmPromotionAnswer = std::function<void(bool confirmed)>;
    using ConfirmPromotionFn = std::function<void(std::string const& presetId, ConfirmPromotionAnswer answer)>;
    void setConfirmPromotionCallback(ConfirmPromotionFn fn);

    void activateAction(std::string_view id);
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

    void applyPromotedPanelSizes(uimodel::LayoutDocument promotedLayout,
                                 uimodel::LayoutComponentStateDocument promotedState);

    void applyLoadedLayout(std::string presetId,
                           uimodel::LayoutDocument document,
                           uimodel::PreparedLayout preparedLayout,
                           uimodel::LayoutComponentStateDocument componentState);
    async::Task<void> loadLayoutWorkflow(std::shared_ptr<ShellLayoutStore> layoutStorePtr,
                                         std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
                                         std::shared_ptr<AppConfigStore> configStorePtr,
                                         std::stop_token stopToken);
    void applyLoadedLayoutWithFaultReporting(std::string presetId,
                                             uimodel::LayoutDocument document,
                                             uimodel::PreparedLayout preparedLayout,
                                             uimodel::LayoutComponentStateDocument componentState);

    Result<> handleEditorSaveRequested(layout::editor::LayoutSaveResult const& result);

    Result<layout::LayoutHost::PreparedTree> prepareHost(uimodel::PreparedLayout const& layout,
                                                         layout::LayoutBuildStateView buildState);

    uimodel::LayoutDocumentLimits const& layoutLimits() const noexcept;

    /// Prepares and commits a replacement against the current shell state, retaining the old tree on rejection.
    void rebuildHost(uimodel::LayoutDocument const& doc);
    void rebuildHost(uimodel::LayoutDocument const& doc, layout::LayoutBuildStateView buildState);

    rt::AppRuntime& _runtime;
    Gtk::Window& _parentWindow;
    layout::ComponentRegistry _registry;
    layout::ActionRegistry _actionRegistry;
    layout::LayoutRuntimeState _runtimeState;
    TrackRowCache* _trackRowCache;
    ImageCache* _imageCache;
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
    PopoverAttachment _outputDevicePopover;
    PopoverAttachment _menuPopover;
    std::unique_ptr<layout::GioActionBridgeSession> _gioBridgeSessionPtr;
    std::vector<async::Subscription> _playbackSubs;
    uimodel::ShellLayoutSessionModel _session;
    std::shared_ptr<AppConfigStore> _configStorePtr;
    std::shared_ptr<ShellLayoutStore> _layoutStorePtr;
    std::shared_ptr<ShellLayoutComponentStateStore> _componentStateStorePtr;
    ThemeCoordinator& _themeCoordinator;
    std::optional<ThemeRegistrationToken> _optEditorThemeToken;
    std::shared_ptr<layout::editor::LayoutEditorDialog> _editorDialogPtr;
    async::LifetimeScope _tasks;
    ConfirmPromotionFn _confirmPromotionFn;
    sigc::scoped_connection _queuedOpenEditorConnection;
    MainContextCallbackScope _callbackScope;
  };
} // namespace ao::gtk
