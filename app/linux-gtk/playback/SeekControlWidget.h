// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/seek/PlaybackPositionInterpolator.h>
#include <ao/uimodel/playback/seek/SeekSliderInteractionModel.h>
#include <ao/uimodel/playback/seek/SeekViewModel.h>

#include <gtkmm/scale.h>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>
#include <sigc++/scoped_connection.h>

#include <chrono>
#include <cstdint>

namespace ao::rt
{
  class PlaybackService;
}

namespace ao::gtk
{
  /**
   * @brief A composite widget providing a seek slider for playback.
   */
  class SeekControlWidget final
  {
  public:
    explicit SeekControlWidget(rt::PlaybackService& playback);
    ~SeekControlWidget();

    SeekControlWidget(SeekControlWidget const&) = delete;
    SeekControlWidget& operator=(SeekControlWidget const&) = delete;
    SeekControlWidget(SeekControlWidget&&) = delete;
    SeekControlWidget& operator=(SeekControlWidget&&) = delete;

    Gtk::Widget& widget() { return _scale; }
    bool isTickActive() const noexcept;

  private:
    void applyState(uimodel::SeekViewState const& view);
    void startTickIfNeeded();
    void stopTick();
    void updateTickState();

    void handleScaleValueChanged();
    void beginUserInteraction();
    void endUserInteraction();
    void applySeekUpdate(uimodel::SeekSliderUpdate const& update);
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
    bool _isMapped = false;
    std::uint32_t _tickId = 0;
    sigc::connection _debounceConnection;
    sigc::scoped_connection _mapConnection;
    sigc::scoped_connection _unmapConnection;
    uimodel::SeekViewModel _seekViewModel;
  };
} // namespace ao::gtk
