// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AudioQualityTooltipController.h"

#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <gtkmm/widget.h>
#include <sigc++/functors/mem_fun.h>

#include <cstdint>

namespace ao::gtk
{
  AudioQualityTooltipController::AudioQualityTooltipController() = default;
  AudioQualityTooltipController::~AudioQualityTooltipController() = default;

  void AudioQualityTooltipController::attach(Gtk::Widget& widget)
  {
    _widget = &widget;
    widget.set_has_tooltip(true);
    widget.signal_query_tooltip().connect(sigc::mem_fun(*this, &AudioQualityTooltipController::onQueryTooltip), false);
  }

  void AudioQualityTooltipController::apply(uimodel::playback::AudioQualityTooltipView const& view)
  {
    _view = view;

    if (_widget != nullptr)
    {
      _widget->trigger_tooltip_query();
    }
  }

  bool AudioQualityTooltipController::onQueryTooltip(std::int32_t /*xCoord*/,
                                                     std::int32_t /*yCoord*/,
                                                     bool /*keyboardTooltip*/,
                                                     Glib::RefPtr<Gtk::Tooltip> const& tooltip)
  {
    if (_view.flow.nodes.empty())
    {
      if (!_view.plainTextFallback.empty())
      {
        tooltip->set_text(_view.plainTextFallback);
        return true;
      }

      return false;
    }

    _tooltipWidget.apply(_view);
    tooltip->set_custom(_tooltipWidget);
    return true;
  }
} // namespace ao::gtk
