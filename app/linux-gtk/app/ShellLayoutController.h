// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/ShellLayoutCollaborators.h"
#include "app/ThemeCoordinator.h"
#include "common/MainContextCallbackScope.h"
#include "common/PopoverAttachment.h"
#include "layout/editor/LayoutEditorDialog.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/GioActionBridge.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutHost.h"
#include <ao/Error.h>
#include <ao/async/LifetimeScope.h>
#include <ao/async/Subscription.h>
#include <ao/async/Task.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/layout/shell/LayoutSession.h>
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>

#include <giomm/menumodel.h>
#include <glibmm/refptr.h>
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

namespace ao::async
{
  class Runtime;
}

namespace ao::uimodel
{
  class ListPresentations;
  class PlaybackActions;
  class TrackPresentationCatalog;
}

namespace ao::gtk
{
  class AobusSoulWindow;
  class AppConfigStore;
  class ResourceImageLoader;
  class ListNavigationController;
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
                                                uimodel::ActionCapabilityMask,
                                                layout::ActionHandler,
                                                layout::ActionStateProvider)>;

    ShellLayoutController(rt::AppRuntime& runtime,
                          Gtk::Window& parentWindow,
                          std::shared_ptr<AppConfigStore> configStorePtr,
                          std::shared_ptr<ShellLayoutStore> layoutStorePtr,
                          std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
                          ShellLayoutCollaborators collaborators);
    ~ShellLayoutController() override;

    ShellLayoutController(ShellLayoutController const&) = delete;
    ShellLayoutController& operator=(ShellLayoutController const&) = delete;
    ShellLayoutController(ShellLayoutController&&) = delete;
    ShellLayoutController& operator=(ShellLayoutController&&) = delete;

    layout::ComponentRegistry& registry() { return _registry; }
    uimodel::LayoutSchema const& layoutSchema() const { return _registry.schema(); }
    uimodel::LayoutSession const& layoutSession() const { return _session; }
    layout::LayoutHost& host() { return _host; }
    uimodel::LayoutDocument const& activeLayout() const { return _session.layout(); }

    void attachToWindow();
    void refreshExportedActions();
    void loadLayout();
    void openEditor(AppConfigStore& configStore);
    void resetRuntimeLayoutState();
    void saveCurrentPanelSizesAsLayoutDefaults();

    using ConfirmPromotionAnswer = std::function<void(bool confirmed)>;
    using ConfirmPromotionFn = std::function<void(std::string const& presetId, ConfirmPromotionAnswer answer)>;
    void setConfirmPromotionCallback(ConfirmPromotionFn fn);

    void activateAction(std::string_view id);
    layout::ActionAvailability actionAvailability(std::string_view id);

    layout::editor::LayoutEditorDialog* editorDialog() const { return _editorDialogPtr.get(); }
    Gtk::Window* soulWindow() const noexcept;

    layout::ActionActivationContext actionContext(std::string_view componentId) override;
    bool canProvideSafeAnchor(uimodel::ActionSchema const& actionSchema) const override;

  private:
    void registerPlaybackActions(RegisterActionFn const& registerAction);
    void registerShellActions(RegisterActionFn const& registerAction);
    void registerWorkspaceActions(RegisterActionFn const& registerAction,
                                  layout::ActionStateProvider const& hasActiveSequence);
    void registerTrackActions(RegisterActionFn const& registerAction);
    void registerTrackOrderActions(RegisterActionFn const& registerAction);
    void presentSoul(layout::ActionActivationContext& context);

    void applyPromotedPanelSizes(uimodel::LayoutDocument promotedLayout,
                                 uimodel::LayoutComponentStateDocument promotedState);

    void applyLoadedLayout(std::string presetId,
                           uimodel::LayoutDocument document,
                           uimodel::PreparedLayout preparedLayout,
                           uimodel::LayoutComponentStateDocument componentState);
    static async::Task<void> loadLayoutWorkflow(ShellLayoutController* controller,
                                                async::Runtime* asyncRuntime,
                                                std::shared_ptr<ShellLayoutStore> layoutStorePtr,
                                                std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
                                                std::shared_ptr<AppConfigStore> configStorePtr,
                                                uimodel::LayoutSchema schema,
                                                std::stop_token stopToken);
    Result<> handleEditorSaveRequested(layout::editor::LayoutSaveResult const& result);

    Result<layout::LayoutHost::PreparedTree> prepareHost(uimodel::PreparedLayout const& layout,
                                                         uimodel::LayoutBuildSnapshot buildSnapshot);

    uimodel::LayoutDocumentLimits const& layoutLimits() const noexcept;

    /// Prepares and commits a replacement against the current shell state, retaining the old tree on rejection.
    void rebuildHost(uimodel::LayoutDocument const& doc);
    void rebuildHost(uimodel::LayoutDocument const& doc, uimodel::LayoutBuildSnapshot buildSnapshot);

    rt::AppRuntime& _runtime;
    Gtk::Window& _parentWindow;
    layout::ComponentRegistry _registry;
    layout::ActionRegistry _actionRegistry;
    i18n::MessageCatalog _textCatalog;
    uimodel::PlaybackActions& _playbackActions;
    ThemeCoordinator& _themeCoordinator;
    TagEditController* _tagEditController = nullptr;
    TrackPageHost* _trackPageHost = nullptr;
    uimodel::OutputDeviceIntent _outputDeviceIntent;
    Glib::RefPtr<Gio::MenuModel> _menuModelPtr;
    layout::LayoutHost _host;
    PopoverAttachment _outputDevicePopover;
    PopoverAttachment _menuPopover;
    std::unique_ptr<layout::GioActionBridgeSession> _gioBridgeSessionPtr;
    std::vector<async::Subscription> _actionStateSubscriptions;
    uimodel::LayoutSession _session;
    std::shared_ptr<AppConfigStore> _configStorePtr;
    std::shared_ptr<ShellLayoutStore> _layoutStorePtr;
    std::shared_ptr<ShellLayoutComponentStateStore> _componentStateStorePtr;
    std::optional<ThemeRegistrationToken> _optEditorThemeToken;
    std::shared_ptr<layout::editor::LayoutEditorDialog> _editorDialogPtr;
    sigc::scoped_connection _queuedEditorDialogRetirementConnection;
    async::LifetimeScope _tasks;
    ConfirmPromotionFn _confirmPromotionFn;
    std::unique_ptr<AobusSoulWindow> _soulWindowPtr;
    sigc::scoped_connection _soulWindowHideConnection;
    sigc::scoped_connection _queuedSoulWindowRetirementConnection;
    bool _soulWindowRetirementQueued = false;
    sigc::scoped_connection _queuedOpenEditorConnection;
    MainContextCallbackScope _callbackScope;
  };
} // namespace ao::gtk
