// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/ThemeCoordinator.h"
#include "layout/editor/LayoutEditorDialog.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/GioActionBridge.h"
#include "layout/runtime/LayoutContext.h"
#include "layout/runtime/LayoutHost.h"
#include <ao/async/LifetimeScope.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/uimodel/layout/action/LayoutActionCatalog.h>
#include <ao/uimodel/layout/action/LayoutActionTypes.h>
#include <ao/uimodel/layout/component/LayoutStatePromoter.h>
#include <ao/uimodel/layout/shell/ShellLayoutSessionModel.h>

#include <gtkmm/window.h>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
}

namespace ao::gtk
{
  class AppConfig;
  class ShellLayoutComponentStateStore;
  class ShellLayoutStore;
  class ThemeCoordinator;
  namespace layout::editor
  {
    class LayoutEditorDialog;
  }

  class ShellLayoutController final : public layout::IActionContextProvider
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
                          std::shared_ptr<AppConfig> configPtr,
                          std::shared_ptr<ShellLayoutStore> layoutStorePtr,
                          std::shared_ptr<ShellLayoutComponentStateStore> componentStateStorePtr,
                          ThemeCoordinator& themeCoordinator);
    ~ShellLayoutController() override;

    ShellLayoutController(ShellLayoutController const&) = delete;
    ShellLayoutController& operator=(ShellLayoutController const&) = delete;
    ShellLayoutController(ShellLayoutController&&) = delete;
    ShellLayoutController& operator=(ShellLayoutController&&) = delete;

    layout::ComponentRegistry& registry() { return _registry; }
    uimodel::LayoutActionCatalog const& actionCatalog() const { return _actionRegistry.catalog(); }
    layout::LayoutContext& context() { return _context; }
    layout::LayoutHost& host() { return _host; }
    uimodel::LayoutDocument const& activeLayout() const { return _session.snapshot().layout; }

    void attachToWindow();
    void refreshExportedActions();
    void loadLayout(AppConfig& config);
    void openEditor(AppConfig& config);
    void resetRuntimeLayoutState();
    void saveCurrentPanelSizesAsLayoutDefaults();

    using ConfirmPromotionAnswer = std::function<void(bool confirmed)>;
    using ConfirmPromotionFn = std::function<void(std::string const& presetId, ConfirmPromotionAnswer answer)>;
    void setConfirmPromotionCallback(ConfirmPromotionFn fn);

    uimodel::LayoutActionActivationOutcome activateAction(std::string_view id);
    uimodel::LayoutActionAvailability actionAvailability(std::string_view id);

    layout::editor::LayoutEditorDialog* editorDialogForTest() const { return _editorDialogPtr.get(); }

    layout::ActionActivationContext getActionContext(std::string_view componentId) override;
    bool canProvideSafeAnchor(uimodel::LayoutActionDescriptor const& desc) const override;

  private:
    void registerPlaybackActions(RegisterActionFn const& registerAction,
                                 layout::ActionStateProvider const& hasActiveQueue);
    void registerShellActions(RegisterActionFn const& registerAction);
    void registerWorkspaceActions(RegisterActionFn const& registerAction,
                                  layout::ActionStateProvider const& hasActiveQueue);
    void registerTrackActions(RegisterActionFn const& registerAction);

    void applyPromotedPanelSizes(std::string const& presetId,
                                 uimodel::LayoutDocument promotedLayout,
                                 uimodel::LayoutComponentStateDocument promotedState);

    void applyLoadedLayout(std::string presetId,
                           uimodel::LayoutDocument document,
                           uimodel::LayoutComponentStateDocument componentState);
    void logLayoutLoadFailure(std::exception_ptr exceptionPtr);
    void applyLoadedLayoutWithFailureLogging(std::string presetId,
                                             uimodel::LayoutDocument document,
                                             uimodel::LayoutComponentStateDocument componentState);

    void onEditorSaveRequest(layout::editor::LayoutSaveResult const& result);

    static void setupCss();

    layout::ComponentRegistry _registry;
    layout::ActionRegistry _actionRegistry;
    layout::LayoutContext _context;
    layout::LayoutHost _host;
    std::unique_ptr<layout::GioActionBridgeSession> _gioBridgeSessionPtr;
    std::vector<rt::Subscription> _playbackSubs;
    uimodel::ShellLayoutSessionModel _session;
    std::shared_ptr<AppConfig> _configPtr;
    std::shared_ptr<ShellLayoutStore> _layoutStorePtr;
    std::shared_ptr<ShellLayoutComponentStateStore> _componentStateStorePtr;
    ThemeCoordinator& _themeCoordinator;
    std::optional<ThemeRegistrationToken> _optEditorThemeToken;
    std::shared_ptr<layout::editor::LayoutEditorDialog> _editorDialogPtr;
    async::LifetimeScope _tasks;
    ConfirmPromotionFn _confirmPromotionFn;
  };
} // namespace ao::gtk
