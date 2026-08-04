// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/component/shell/PaneSplitter.h"

#include "pch.h"

#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.UI.h>

#include <functional>
#include <utility>

namespace ao::winui::layout
{
  namespace
  {
    using winrt::Microsoft::UI::Xaml::Visibility;
    using winrt::Microsoft::UI::Xaml::Media::SolidColorBrush;

    /// Wide enough to grab at every supported scale, narrow enough to read as a boundary.
    constexpr double kSplitterWidth = 6.0;
  } // namespace

  PaneSplitter::PaneSplitter(PaneEdge const edge, std::function<void(double)> onResize, std::function<void()> onCommit)
    : _edge{edge}, _onResize{std::move(onResize)}, _onCommit{std::move(onCommit)}
  {
    _thumb.Width(kSplitterWidth);
    // A transparent brush keeps the boundary hit-testable while the pane edges
    // on either side of it carry the visible chrome.
    _thumb.Background(SolidColorBrush{winrt::Windows::UI::Colors::Transparent()});
    _dragDeltaRevoker = _thumb.DragDelta(winrt::auto_revoke, {this, &PaneSplitter::onDragDelta});
    _dragCompletedRevoker = _thumb.DragCompleted(winrt::auto_revoke, {this, &PaneSplitter::onDragCompleted});
  }

  void PaneSplitter::setVisible(bool const visible)
  {
    _thumb.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
  }

  void PaneSplitter::onDragDelta(winrt::Windows::Foundation::IInspectable const& /*sender*/,
                                 winrt::Microsoft::UI::Xaml::Controls::Primitives::DragDeltaEventArgs const& args)
  {
    if (_onResize)
    {
      _onResize(_edge == PaneEdge::Leading ? -args.HorizontalChange() : args.HorizontalChange());
    }
  }

  void PaneSplitter::onDragCompleted(
    winrt::Windows::Foundation::IInspectable const& /*sender*/,
    winrt::Microsoft::UI::Xaml::Controls::Primitives::DragCompletedEventArgs const& /*args*/)
  {
    if (_onCommit)
    {
      _onCommit();
    }
  }
} // namespace ao::winui::layout
