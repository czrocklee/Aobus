// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "layout/runtime/LayoutComponent.h"
#include <ao/Error.h>
#include <ao/uimodel/layout/shell/ShellGenerationSequence.h>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <memory>

namespace ao::winui::layout
{
  class FocusedDetail;

  /// One complete Windows view generation built from one document.
  struct ShellGeneration final
  {
    std::unique_ptr<LayoutComponent> rootPtr;
    std::shared_ptr<uimodel::ShellGenerationGate> gatePtr;

    /**
     * @brief The focused view's detail projection, shared by the whole generation.
     *
     * Held here so it outlives no generation but its own, and held through the
     * holder rather than directly so the components that share it can be moved
     * to the next runtime's projection together.
     */
    std::shared_ptr<FocusedDetail> focusedDetailPtr;

    /**
     * @brief The element the frame offers the system as its title bar, if the preset has one.
     *
     * Only the window can hand a drag region to the system, and only the
     * document knows which of its nodes is one, so the generation records what
     * it built and the frame asks for it after each publication.
     */
    winrt::Microsoft::UI::Xaml::FrameworkElement titleBarElement{nullptr};
  };

  /**
   * @brief The window's single layout host.
   *
   * Owns at most one live generation. A candidate is constructed and bound
   * while the current generation stays visible, then published as one unit: the
   * host attaches the candidate root and changes the active generation token
   * exactly once. An unexpected attachment failure restores the previous root
   * and token before reporting it, and the candidate is destroyed.
   *
   * The host does not build or update generations. It owns only publication
   * and retirement; components subscribe directly to shell-lifetime sources.
   */
  class LayoutHost final
  {
  public:
    explicit LayoutHost(winrt::Microsoft::UI::Xaml::Controls::Border host);
    ~LayoutHost();

    LayoutHost(LayoutHost const&) = delete;
    LayoutHost& operator=(LayoutHost const&) = delete;
    LayoutHost(LayoutHost&&) = delete;
    LayoutHost& operator=(LayoutHost&&) = delete;

    /// Open a gate for a candidate that is about to be constructed.
    std::shared_ptr<uimodel::ShellGenerationGate> stage();

    /// Publish @p candidate and destroy the generation it replaces.
    Result<> publish(ShellGeneration candidate);

    /// Abandon a candidate whose construction or binding failed.
    void discard(ShellGeneration candidate);

    /// Whether a generation is currently live.
    bool hasActiveGeneration() const noexcept { return _active.rootPtr != nullptr; }

    /// The live generation's title bar element, or nothing when its preset has none.
    winrt::Microsoft::UI::Xaml::FrameworkElement activeTitleBar() const { return _active.titleBarElement; }

    /// Detach and destroy the live generation.
    void retire() noexcept;

  private:
    winrt::Microsoft::UI::Xaml::Controls::Border _host{nullptr};
    uimodel::ShellGenerationSequence _sequence;
    ShellGeneration _active;
  };
} // namespace ao::winui::layout
