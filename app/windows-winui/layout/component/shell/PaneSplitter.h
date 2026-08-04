// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

#include <cstdint>
#include <functional>

namespace ao::winui::layout
{
  /// Which edge of its pane a splitter sits on, and therefore which drag direction widens it.
  enum class PaneEdge : std::uint8_t
  {
    /// The pane extends to the right of the boundary, so dragging left widens it.
    Leading,
    /// The pane extends to the left of the boundary, so dragging right widens it.
    Trailing,
  };

  /**
   * @brief The boundary the user drags to resize a shell pane.
   *
   * A persisted pane width belongs to the pane, so the splitter reports only the
   * signed width change its edge implies and leaves clamping, application, and
   * persistence to its owner. Containers around a pane allocate slots and never
   * learn what a drag means.
   */
  class PaneSplitter final
  {
  public:
    PaneSplitter(PaneEdge edge, std::function<void(double)> onResize, std::function<void()> onCommit);

    PaneSplitter(PaneSplitter const&) = delete;
    PaneSplitter& operator=(PaneSplitter const&) = delete;
    PaneSplitter(PaneSplitter&&) = delete;
    PaneSplitter& operator=(PaneSplitter&&) = delete;
    ~PaneSplitter() = default;

    winrt::Microsoft::UI::Xaml::Controls::Primitives::Thumb element() const noexcept { return _thumb; }

    /// A boundary the user cannot act on is hidden rather than merely inert.
    void setVisible(bool visible);

  private:
    void onDragDelta(winrt::Windows::Foundation::IInspectable const& sender,
                     winrt::Microsoft::UI::Xaml::Controls::Primitives::DragDeltaEventArgs const& args);
    void onDragCompleted(winrt::Windows::Foundation::IInspectable const& sender,
                         winrt::Microsoft::UI::Xaml::Controls::Primitives::DragCompletedEventArgs const& args);

    winrt::Microsoft::UI::Xaml::Controls::Primitives::Thumb _thumb{};
    PaneEdge _edge = PaneEdge::Trailing;
    std::function<void(double)> _onResize;
    std::function<void()> _onCommit;
    winrt::Microsoft::UI::Xaml::Controls::Primitives::Thumb::DragDelta_revoker _dragDeltaRevoker{};
    winrt::Microsoft::UI::Xaml::Controls::Primitives::Thumb::DragCompleted_revoker _dragCompletedRevoker{};
  };
} // namespace ao::winui::layout
