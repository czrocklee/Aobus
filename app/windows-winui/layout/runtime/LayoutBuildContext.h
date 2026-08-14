// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/ActionRegistry.h"
#include "layout/runtime/CommonLayoutProps.h"
#include "layout/runtime/FocusedDetail.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/ShellLibraryAccess.h"
#include <ao/async/Signal.h>
#include <ao/uimodel/layout/shell/LayoutBuildStateView.h>
#include <ao/winui/layout/ShellStatePolicy.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ao::rt
{
  class LibraryTaskService;
  class NotificationService;
  class PlaybackService;
  class ResourceByteLoader;
  class ViewService;
  class WorkspaceService;
}

namespace ao::audio
{
  struct OutputDeviceSelection;
}

namespace ao::uimodel
{
  class LayoutComponentCatalog;
  class PlaybackCommandSurface;
  class ShellGenerationGate;
  struct LayoutRuntimeState;
} // namespace ao::uimodel

namespace ao::winui
{
  class ThemeCoordinator;
  class TrackListController;
}

namespace ao::async
{
  class Runtime;
}

namespace ao::winui::layout
{
  /// Whether the native window currently permits UI work such as animation.
  struct WindowActivityState final
  {
    bool visible = true;
    bool minimized = false;

    friend bool operator==(WindowActivityState const&, WindowActivityState const&) = default;
  };

  /**
   * @brief Persistent Windows pane sizes a shell component reads and writes.
   *
   * Navigation and inspector boundaries belong to `DesktopSettings`,
   * which atomically owns them together with window, shell, and track-view
   * state. Components receive that authority through these accessors instead of
   * mapping it into the generic component-state store, which cannot express
   * participation in the shared settings candidate.
   */
  struct PaneSettingsAccess final
  {
    std::function<double()> navigationWidth;
    std::function<void(double)> setNavigationWidth;
    std::function<double()> inspectorWidth;
    std::function<void(double)> setInspectorWidth;
    /// Persist the current settings candidate after a completed drag.
    std::function<void()> commit;
  };

  /**
   * @brief The shell's menu composition, borrowed by the components that present it.
   *
   * Menus belong to the shell, which owns the commands their items invoke; a
   * component only presents what it is handed. A missing composer means the
   * shell offers no menus and therefore rejects a document that asks for one.
   */
  struct MenuComposer final
  {
    /// The flyout a `menuButton` presents, keyed by its authored `menuId`.
    std::function<winrt::Microsoft::UI::Xaml::Controls::MenuFlyout(std::string_view)> flyout;

    /// Fills the application menu bar the Classic shell shows.
    std::function<void(winrt::Microsoft::UI::Xaml::Controls::MenuBar const&)> composeMenuBar;
  };

  /**
   * @brief Everything one Windows component build may borrow.
   *
   * Assembled per build and never retained: a component keeps only the narrow
   * references it needs, because the bundle is borrowed from the window and
   * the context is rebuilt for each generation.
   */
  struct LayoutBuildContext final
  {
    /// Explicit borrowed capabilities for the currently active library session.
    async::Runtime& asyncRuntime;
    rt::PlaybackService& playback;
    rt::ViewService& views;
    rt::WorkspaceService& workspace;
    rt::NotificationService& notifications;
    rt::LibraryTaskService& libraryTasks;
    uimodel::PlaybackCommandSurface& playbackCommands;
    TrackListController& trackList;
    rt::ResourceByteLoader& resourceBytes;
    ThemeCoordinator& theme;
    ShellLibraryAccess const& library;

    /// Decides which types exist and which action slots each of them accepts.
    uimodel::LayoutComponentCatalog const& catalog;

    /// Shell-lifetime action handlers every bound slot resolves against.
    ActionRegistry const& actions;

    /// The window's own resource scope; the only scope `styleKey` resolves against.
    winrt::Microsoft::UI::Xaml::ResourceDictionary resources{nullptr};

    /// Turns an authored `surface` slot into the brush the active theme paints it with.
    SurfaceBrushResolver surfaceBrush;

    /// Shell-lifetime component runtime state and the per-build view over it.
    uimodel::LayoutRuntimeState& runtimeState;
    uimodel::LayoutBuildStateView buildState;

    /**
     * @brief Current shell state and generation-scoped change delivery.
     *
     * Components read the current value while they are built and retain only
     * the subscription returned by the signal. The subscription then dies with
     * the component, before the window-owned source.
     */
    ShellState const& shellState;
    async::Signal<ShellState>& shellStateChanged;

    /// Current native-window activity and changes committed after this build.
    WindowActivityState const& windowActivity;
    async::Signal<WindowActivityState>& windowActivityChanged;

    /// Current transient message and changes committed after this build.
    std::string const& statusMessage;
    async::Signal<std::string>& statusMessageChanged;

    /// The generation being constructed. Callbacks capture it weakly.
    std::shared_ptr<uimodel::ShellGenerationGate> gatePtr;

    PaneSettingsAccess paneSettings;

    /// Records the exact route requested through a generation-owned selector.
    std::function<void(audio::OutputDeviceSelection const&)> onOutputDeviceSelectionRequested{};

    MenuComposer menus;

    /**
     * @brief Where a component reports a transient shell message.
     *
     * The status surface is itself a component, and therefore belongs to the
     * generation, so reports travel through the shell rather than between
     * components: any generation's message reaches whichever status component
     * is live when it arrives.
     */
    std::function<void(std::string)> reportStatus;

    /**
     * @brief The generation's shared focused-detail projection.
     *
     * Components ask this holder for a projection using `workspace` above.
     * The projection dies with the generation that made it.
     */
    std::shared_ptr<FocusedDetail> focusedDetailPtr;

    /// Where the preset's title bar, if it authored one, records itself for the frame.
    winrt::Microsoft::UI::Xaml::FrameworkElement& titleBarSlot;
  };
} // namespace ao::winui::layout
