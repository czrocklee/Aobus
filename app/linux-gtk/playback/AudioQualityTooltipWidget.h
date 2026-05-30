// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <gtkmm/box.h>

namespace ao::gtk
{
  class AudioQualityTooltipWidget final : public Gtk::Box
  {
  public:
    AudioQualityTooltipWidget();
    ~AudioQualityTooltipWidget() override = default;

    AudioQualityTooltipWidget(AudioQualityTooltipWidget const&) = delete;
    AudioQualityTooltipWidget& operator=(AudioQualityTooltipWidget const&) = delete;
    AudioQualityTooltipWidget(AudioQualityTooltipWidget&&) = delete;
    AudioQualityTooltipWidget& operator=(AudioQualityTooltipWidget&&) = delete;

    void apply(uimodel::playback::AudioQualityTooltipView const& view);
  };
} // namespace ao::gtk
