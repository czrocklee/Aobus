// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/CommonLayoutProps.h"
#include "layout/runtime/FocusedDetail.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/winui/layout/ShellState.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ao::uimodel
{
  class ShellGenerationGate;
} // namespace ao::uimodel

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
   * Assembled per build and never retained. Session-lifetime collaborators are
   * captured by component factories when registered; this context carries only
   * the values that vary with the generation being constructed.
   */
  struct LayoutBuildContext final
  {
    /// The window's own resource scope; the only scope `styleKey` resolves against.
    winrt::Microsoft::UI::Xaml::ResourceDictionary resources{nullptr};

    /// Turns an authored `surface` slot into the brush the active theme paints it with.
    SurfaceBrushResolver surfaceBrush;

    /**
     * @brief Current shell state captured for the generation being built.
     */
    ShellState const& shellState;

    /// Current native-window activity at build time.
    WindowActivityState const& windowActivity;

    /// Current transient message at build time.
    std::string const& statusMessage;

    /// The generation being constructed. Callbacks capture it weakly.
    std::shared_ptr<uimodel::ShellGenerationGate> gatePtr;

    /// Records the exact route requested through a generation-owned selector.
    uimodel::OutputDeviceIntent outputDeviceIntent;

    /**
     * @brief The generation's shared focused-detail projection.
     *
     * The registered factory resolves this projection from its captured
     * workspace service. The projection dies with the generation that made it.
     */
    std::shared_ptr<FocusedDetail> focusedDetailPtr;

    /// Where the preset's title bar, if it authored one, records itself for the frame.
    winrt::Microsoft::UI::Xaml::FrameworkElement& titleBarSlot;
  };
} // namespace ao::winui::layout
