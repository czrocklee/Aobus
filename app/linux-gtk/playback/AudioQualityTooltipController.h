// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "playback/AudioQualityTooltipWidget.h"
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <gtkmm/tooltip.h>
#include <gtkmm/widget.h>

#include <cstdint>

namespace ao::gtk
{
  class AudioQualityTooltipController final
  {
  public:
    AudioQualityTooltipController();
    ~AudioQualityTooltipController();

    AudioQualityTooltipController(AudioQualityTooltipController const&) = delete;
    AudioQualityTooltipController& operator=(AudioQualityTooltipController const&) = delete;
    AudioQualityTooltipController(AudioQualityTooltipController&&) = delete;
    AudioQualityTooltipController& operator=(AudioQualityTooltipController&&) = delete;

    void attach(Gtk::Widget& widget);
    void apply(uimodel::playback::AudioQualityTooltipView const& view);

  private:
    bool onQueryTooltip(std::int32_t xCoord,
                        std::int32_t yCoord,
                        bool keyboardTooltip,
                        Glib::RefPtr<Gtk::Tooltip> const& tooltip);

    uimodel::playback::AudioQualityTooltipView _view;
    AudioQualityTooltipWidget _tooltipWidget;
    Gtk::Widget* _widget = nullptr;
  };
} // namespace ao::gtk
