// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/SurfaceBrushResolver.h"
#include <ao/uimodel/playback/output/OutputDeviceIntent.h>
#include <ao/winui/layout/ShellState.h>

#include <winrt/Microsoft.UI.Xaml.h>

#include <memory>
#include <string>

namespace ao::uimodel
{
  class ShellGenerationGate;
} // namespace ao::uimodel

namespace ao::winui::layout
{
  class FocusedDetail;

  /// Whether the native window currently permits UI work such as animation.
  struct WindowActivityState final
  {
    bool visible = true;
    bool minimized = false;

    friend bool operator==(WindowActivityState const&, WindowActivityState const&) = default;
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
