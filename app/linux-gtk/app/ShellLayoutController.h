// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/ThemeCoordinator.h"
#include "layout/document/LayoutDocument.h"
#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/GioActionBridge.h"
#include "layout/runtime/LayoutContext.h"
#include "layout/runtime/LayoutHost.h"
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/async/LifetimeScope.h>

#include <gtkmm/window.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk
{
  class AppConfig;
  class ThemeCoordinator;

  class ShellLayoutController final : public layout::IActionContextProvider
  {
  public:
    using RegisterActionFn = std::function<void(std::string_view,
                                                std::string_view,
                                                std::string_view,
                                                layout::ActionCapabilities,
                                                layout::ActionHandler,
                                                layout::ActionStateProvider)>;

    ShellLayoutController(rt::AppRuntime& runtime,
                          Gtk::Window& parentWindow,
                          std::shared_ptr<AppConfig> configPtr,
                          ThemeCoordinator& themeCoordinator);

    layout::ComponentRegistry& registry() { return _registry; }
    layout::LayoutContext& context() { return _context; }
    layout::LayoutHost& host() { return _host; }
    layout::LayoutDocument const& activeLayout() const { return _activeLayout; }

    void attachToWindow();
    void refreshExportedActions();
    void loadLayout(AppConfig& config);
    void saveLayout(AppConfig& config) const;
    void openEditor(AppConfig& config);

    layout::ActionActivationContext getActionContext(std::string_view componentId) override;
    bool canProvideSafeAnchor(layout::ActionDescriptor const& desc) const override;

  private:
    void registerPlaybackActions(RegisterActionFn const& registerAction,
                                 layout::ActionStateProvider const& hasActiveQueue);
    void registerShellActions(RegisterActionFn const& registerAction);
    void registerWorkspaceActions(RegisterActionFn const& registerAction,
                                  layout::ActionStateProvider const& hasActiveQueue);
    void registerTrackActions(RegisterActionFn const& registerAction);

    static void setupCss();

    layout::ComponentRegistry _registry;
    layout::ActionRegistry _actionRegistry;
    layout::LayoutContext _context;
    layout::LayoutHost _host;
    std::unique_ptr<layout::GioActionBridgeSession> _gioBridgeSessionPtr;
    std::vector<rt::Subscription> _playbackSubs;
    layout::LayoutDocument _activeLayout;
    std::string _activePresetId;
    std::shared_ptr<AppConfig> _configPtr;
    ThemeCoordinator& _themeCoordinator;
    std::optional<ThemeRegistrationToken> _optEditorThemeToken;
    bool _isCustomized = false;
    rt::async::LifetimeScope _tasks;
  };
} // namespace ao::gtk
