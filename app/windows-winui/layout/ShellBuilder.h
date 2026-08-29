// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/ComponentRegistry.h"
#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutHost.h"
#include <ao/Error.h>
#include <ao/async/Signal.h>
#include <ao/uimodel/layout/component/LayoutSchema.h>
#include <ao/uimodel/library/list/ListOrder.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>
#include <ao/winui/Theme.h>
#include <ao/winui/layout/ShellDocument.h>
#include <ao/winui/layout/ShellStatePolicy.h>
#include <ao/winui/list/ListAuthoringAdapter.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ao::winui
{
  class LibrarySession;
  class ThemeCoordinator;
  class TrackListController;
}

namespace ao::rt
{
  class ResourceByteMemoryCache;
}

namespace ao::winui::layout
{
  /**
   * @brief The window-frame capabilities a document's actions and menus invoke.
   *
   * A preset names commands; only the frame can run them, because they open
   * pickers, raise system menus, or change what the frame itself presents. They
   * are supplied once and outlive every generation, which is what lets a
   * generation be replaced without re-registering behavior.
   */
  struct ShellCommands final
  {
    std::function<void()> openLibrary;
    std::function<void()> rescanLibrary;
    std::function<void()> importLibrary;
    std::function<void()> exportLibrary;
    std::function<void()> toggleInspector;
    std::function<void()> toggleShellMode;
    std::function<void()> chooseColumns;
    std::function<void()> reloadTheme;
    std::function<void()> playPause;
    std::function<void()> stop;
    std::function<void()> revealCurrentTrack;
    std::function<void()> presentTrackProperties;
    std::function<void()> showSoul;
    std::function<void()> showSystemMenu;
    std::function<void(winrt::Microsoft::UI::Xaml::FrameworkElement const&)> showOutputDeviceSelector;
  };

  /// Window-owned List workflows exposed to generation components as narrow callbacks.
  struct ShellListCommands final
  {
    std::function<void(ListId, std::string)> createList;
    std::function<void(ListId)> editList;
    std::function<void(ListId, bool)> deleteList;
    std::function<std::vector<uimodel::WritableTagListTarget>()> membershipTargets;
    std::function<void(ListId, bool)> editMembership;
    std::function<uimodel::ListOrderCapabilityState()> orderCapabilities;
    std::function<void(ListOrderCommand)> applyOrder;
  };

  /// What the window frame lends the builder for the life of the shell.
  struct ShellBuilderConfig final
  {
    /// The frame region one generation at a time is attached to.
    winrt::Microsoft::UI::Xaml::Controls::Border host{nullptr};

    /// The window's own resource scope; the only scope `styleKey` resolves against.
    winrt::Microsoft::UI::Xaml::ResourceDictionary resources{nullptr};

    /// Coordinator-owned collaborators borrowed by every shell generation.
    TrackListController& trackList;
    rt::ResourceByteMemoryCache& resourceBytes;
    ThemeCoordinator& theme;

    /// The loaded theme override, or nothing while the system theme is in force.
    std::function<std::optional<Theme>()> activeTheme;

    /// Persist the settings candidate the pane widths participate in.
    std::function<void()> saveSettings;

    ShellCommands commands;
    ShellListCommands listCommands;
  };

  /**
   * @brief Builds, publishes, and drives the document-built Windows shell.
   *
   * Everything a component may borrow that outlives a generation lives here:
   * the action registry, the menu composition, the persistent pane accessors,
   * the runtime component state, and the transient status message. A generation
   * is built from exactly one preset, so the builder rebuilds when the resolved
   * shell mode names the other one. Components read current shell state and
   * retain scoped subscriptions for later changes.
   *
   * A failed build leaves the current generation untouched and reports why. A
   * failed *first* build has no generation to fall back to, so the frame shows
   * a minimal layout-error surface instead: a shipped document that does not
   * build is an artifact defect, not a state the user recovers from.
   */
  class ShellBuilder final
  {
  public:
    ShellBuilder(LibrarySession& session, ShellBuilderConfig config);
    ~ShellBuilder();

    ShellBuilder(ShellBuilder const&) = delete;
    ShellBuilder& operator=(ShellBuilder const&) = delete;
    ShellBuilder(ShellBuilder&&) = delete;
    ShellBuilder& operator=(ShellBuilder&&) = delete;

    /**
     * @brief Resolve and commit shell state, building its preset when needed.
     *
     * Candidate construction sees the last committed state. A resolved change
     * is published only after a required preset has built successfully, so a
     * failed switch leaves both the current generation and its state unchanged.
     */
    Result<ShellState> applyShellState(ShellMode mode, double width, std::optional<bool> optInspectorRequest);

    /// Rebuild the live preset against the current theme and runtime.
    Result<> rebuild();

    /// Publish the native window's current visibility and minimized state.
    void applyWindowActivity(bool visible, bool minimized);

    /// Retain @p message as the shell's transient status and publish a change.
    void reportStatus(std::string message);

    /// The last successfully committed shell state.
    ShellState const& shellState() const noexcept { return _shellState; }

    /// The live generation's title bar, or nothing when its preset authored none.
    winrt::Microsoft::UI::Xaml::FrameworkElement titleBar() const { return _host.activeTitleBar(); }

    /// Invoke one shell-lifetime action from fixed window chrome or an accelerator.
    bool invokeAction(std::string_view actionId) const;

  private:
    /**
     * @brief Close generation admission and detach the native root.
     *
     * Reachable only from the destructor: releasing the builder is what retires
     * it. The step order here is the reason this is not left to member
     * destruction, which would run it backwards.
     */
    void retire() noexcept;

    Result<> build(ShellPreset preset);
    void registerActions();
    void registerComponents();
    void installKeyboardAccelerators();
    PaneSettingsAccess paneSettings();
    MenuComposer menus();
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyout modernOverflowFlyout() const;
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyout nowPlayingOverflowFlyout() const;
    void composeMenuBar(winrt::Microsoft::UI::Xaml::Controls::MenuBar const& bar) const;
    /// Replace the host content with the frame's minimal fatal layout-error surface.
    void showFatalLayoutError(Error const& error);

    LibrarySession& _session;
    ShellBuilderConfig _config;
    uimodel::LayoutSchema _schema;
    ActionRegistry _actions;
    ComponentRegistry _registry;
    // These sources outlive the host so every component subscription disconnects
    // before the signal it observes is destroyed.
    ShellState _shellState = ShellStatePolicy::resolve(ShellMode::Modern, ShellStatePolicy::kWideWidth, std::nullopt);
    async::Signal<ShellState> _shellStateChanged;
    WindowActivityState _windowActivity;
    async::Signal<WindowActivityState> _windowActivityChanged;
    std::string _statusMessage;
    async::Signal<std::string> _statusMessageChanged;

    LayoutHost _host;
    /// The preset the live generation was built from, if any.
    std::optional<ShellPreset> _optLivePreset;
    /**
     * @brief A preset whose build already failed and was already reported.
     *
     * The resolved policy is re-applied on every resize, so without this a
     * broken document would be parsed, rejected, and reported again for each
     * pixel the window moves. Nothing about a resize makes it buildable.
     */
    std::optional<ShellPreset> _optRejectedPreset;
    /**
     * @brief Proof that this builder is still alive, for handlers that outlive it.
     *
     * Keyboard accelerators hang on the frame's host region, which outlives
     * every generation and this builder with it. Construction installs them, so
     * a throw part-way through leaves installed handlers behind with no
     * destructor to run. Holding the token by weak reference is what makes
     * those handlers inert instead of a call into freed memory.
     */
    std::shared_ptr<std::monostate> _lifetimePtr = std::make_shared<std::monostate>();
    bool _retired = false;
  };
} // namespace ao::winui::layout
