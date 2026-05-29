// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/PlaybackDetailsWidget.h"

#include "layout/LayoutConstants.h"
#include <ao/rt/PlaybackService.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <gtkmm/image.h>
#include <gtkmm/label.h>

#include <memory>
#include <string>

namespace ao::gtk
{
  namespace
  {
    void clearSinkStatusClasses(Gtk::Image& image)
    {
      for (auto const& cls : {"ao-quality-perfect",
                              "ao-quality-lossless",
                              "ao-quality-intervention",
                              "ao-quality-lossy",
                              "ao-quality-clipped"})
      {
        image.remove_css_class(cls);
      }
    }

    char const* categoryToCssClass(uimodel::playback::AudioQualityCategory category)
    {
      using Category = uimodel::playback::AudioQualityCategory;

      switch (category)
      {
        case Category::Perfect: return "ao-quality-perfect";
        case Category::Lossless: return "ao-quality-lossless";
        case Category::Intervention: return "ao-quality-intervention";
        case Category::Lossy: return "ao-quality-lossy";
        case Category::Clipped: return "ao-quality-clipped";
        case Category::Unknown: return "";
      }

      return "";
    }
  }

  PlaybackDetailsWidget::PlaybackDetailsWidget(rt::PlaybackService& playbackService)
  {
    _container.set_spacing(layout::kSpacingLarge);
    _container.add_css_class("ao-playback-details");

    _streamInfoLabel.add_css_class("dim-label");
    _sinkStatusIcon.set_from_icon_name("media-record-symbolic");
    _sinkStatusIcon.set_pixel_size(layout::kIconSizeXSmall);
    _sinkStatusIcon.set_visible(false);

    _container.append(_streamInfoLabel);
    _container.append(_sinkStatusIcon);

    _controllerPtr = std::make_unique<ao::uimodel::playback::NowPlayingViewModel>(
      playbackService, [this](ao::uimodel::playback::NowPlayingViewState const& view) { applyState(view); });
  }

  PlaybackDetailsWidget::~PlaybackDetailsWidget() = default;

  void PlaybackDetailsWidget::applyState(ao::uimodel::playback::NowPlayingViewState const& view)
  {
    _streamInfoLabel.set_text(view.streamInfo);

    if (view.pipelineTooltip != _lastTooltipText)
    {
      _container.set_tooltip_text(view.pipelineTooltip);
      _streamInfoLabel.set_tooltip_text(view.pipelineTooltip);
      _sinkStatusIcon.set_tooltip_text(view.pipelineTooltip);
      _lastTooltipText = view.pipelineTooltip;
    }

    clearSinkStatusClasses(_sinkStatusIcon);
    _sinkStatusIcon.set_visible(view.isActive);

    auto const* const cssClass = categoryToCssClass(view.qualityCategory);

    if (cssClass[0] != '\0')
    {
      _sinkStatusIcon.add_css_class(cssClass);
    }
  }
} // namespace ao::gtk
