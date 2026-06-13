// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/PlaybackPositionInterpolator.h>
#include <ao/uimodel/playback/SeekViewModel.h>

#include <gtkmm/scale.h>
#include <gtkmm/widget.h>
#include <sigc++/connection.h>

#include <chrono>
#include <cstdint>

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
    enum class InteractionState : std::uint8_t
    {
      Idle,
      Pointer,
    };

    void applyState(uimodel::playback::SeekViewState const& view);

    void handleScaleValueChanged();
    void beginUserInteraction();
    void endUserInteraction();
    void previewSeekFromScale();
    void commitSeekFromScale();
    void executeDebouncedFinalSeek();
    void setScaleRange(std::chrono::milliseconds duration);
    void setScaleValue(std::chrono::milliseconds elapsed);
    std::chrono::milliseconds scaleElapsed() const noexcept;
    void reset();

    Gtk::Scale _scale;
    uimodel::playback::PlaybackPositionInterpolator _interpolator;

    std::chrono::milliseconds _duration{0};
    InteractionState _interactionState = InteractionState::Idle;
    bool _pendingFinalSeek = false;
    bool _updatingScale = false;
    sigc::connection _debounceConnection;
    uimodel::playback::SeekViewModel _controller;

    friend class test::SeekControlTestPeer;
  };
} // namespace ao::gtk
