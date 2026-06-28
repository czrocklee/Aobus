// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/seek/PlaybackPositionInterpolator.h>
#include <ao/uimodel/playback/seek/SeekSliderInteractionModel.h>
#include <ao/uimodel/playback/seek/SeekViewModel.h>

#include <gtkmm/scale.h>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>

#include <chrono>

namespace ao::gtk::test
{
  class SeekControlTestPeer;
}

namespace ao::gtk
{
  /**
   * @brief A composite widget providing a seek slider for playback.
   */
  class SeekControl final
  {
  public:
    explicit SeekControl(rt::PlaybackService& playbackService);
    ~SeekControl();

    SeekControl(SeekControl const&) = delete;
    SeekControl& operator=(SeekControl const&) = delete;
    SeekControl(SeekControl&&) = delete;
    SeekControl& operator=(SeekControl&&) = delete;

    Gtk::Widget& widget() { return _scale; }

  private:
    void applyState(uimodel::SeekViewState const& view);

    void handleScaleValueChanged();
    void beginUserInteraction();
    void endUserInteraction();
    void applySeekDecision(uimodel::SeekSliderDecision const& decision);
    void commitSeekFromScale();
    void executeDebouncedFinalSeek();
    void setScaleRange(std::chrono::milliseconds duration);
    void setScaleValue(std::chrono::milliseconds elapsed);
    std::chrono::milliseconds scaleElapsed() const noexcept;
    void reset();

    Gtk::Scale _scale;
    uimodel::PlaybackPositionInterpolator _interpolator;
    uimodel::SeekSliderInteractionModel _interaction;

    bool _updatingScale = false;
    sigc::connection _debounceConnection;
    uimodel::SeekViewModel _controller;

    friend class test::SeekControlTestPeer;
  };
} // namespace ao::gtk
