// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PlaybackDetailsWidget.h"

#include "layout/LayoutConstants.h"
#include "playback/AudioQualityCss.h"
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <gtkmm/image.h>
#include <gtkmm/label.h>

namespace ao::gtk
{
  namespace
  {
  }

  PlaybackDetailsWidget::PlaybackDetailsWidget(rt::PlaybackService& playbackService)
    : _controller{playbackService, [this](ao::uimodel::NowPlayingViewState const& view) { applyState(view); }}
  {
    _container.set_spacing(layout::kSpacingLarge);
    _container.add_css_class("ao-playback-details");

    _streamInfoLabel.add_css_class("dim-label");
    _sinkStatusIcon.set_from_icon_name("media-record-symbolic");
    _sinkStatusIcon.set_pixel_size(layout::kIconSizeXSmall);
    _sinkStatusIcon.set_visible(false);

    _container.append(_streamInfoLabel);
    _container.append(_sinkStatusIcon);
  }

  PlaybackDetailsWidget::~PlaybackDetailsWidget() = default;

  void PlaybackDetailsWidget::applyState(ao::uimodel::NowPlayingViewState const& view)
  {
    _streamInfoLabel.set_text(view.streamInfo);

    clearQualityCssClasses(_sinkStatusIcon);
    _sinkStatusIcon.set_visible(view.isActive);

    auto const* const cssClass = qualityCssClass(view.qualityCategory);

    if (cssClass[0] != '\0')
    {
      _sinkStatusIcon.add_css_class(cssClass);
    }
  }
} // namespace ao::gtk
