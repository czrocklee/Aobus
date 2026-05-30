// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AudioQualityTooltipWidget.h"

#include "playback/AudioQualityCss.h"
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/uimodel/playback/AudioQualityFormat.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/separator.h>

#include <algorithm>

namespace ao::gtk
{
  AudioQualityTooltipWidget::AudioQualityTooltipWidget()
    : Gtk::Box{Gtk::Orientation::VERTICAL}
  {
    constexpr int kSpacing = 6;
    set_spacing(kSpacing);
    add_css_class("ao-quality-tooltip");
  }

  void AudioQualityTooltipWidget::apply(uimodel::playback::AudioQualityTooltipView const& view)
  {
    // Remove all existing children
    while (auto* child = get_first_child())
    {
      remove(*child);
    }

    if (view.flow.nodes.empty())
    {
      return;
    }

    // Title label
    auto* titleLabel = Gtk::make_managed<Gtk::Label>("Audio Pipeline", Gtk::Align::START);
    titleLabel->add_css_class("dim-label");
    titleLabel->set_margin_bottom(4);
    append(*titleLabel);

    auto const path = uimodel::playback::playbackPath(view.flow);

    for (auto const* node : path)
    {
      auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
      row->set_spacing(2);

      // Node header
      auto* headerBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      headerBox->set_spacing(4);

      auto const typeLabel = uimodel::playback::audioNodeTypeLabel(node->type);
      auto* typeWidget = Gtk::make_managed<Gtk::Label>(typeLabel);
      typeWidget->add_css_class("dim-label");
      headerBox->append(*typeWidget);

      auto* nameWidget = Gtk::make_managed<Gtk::Label>(node->name);
      headerBox->append(*nameWidget);

      // Format details
      if (node->optFormat)
      {
        auto const formatStr = std::string{"("} + uimodel::playback::audioFormatLabel(*node->optFormat) + ")";
        auto* formatLabel = Gtk::make_managed<Gtk::Label>(formatStr);
        formatLabel->add_css_class("dim-label");
        headerBox->append(*formatLabel);
      }

      row->append(*headerBox);

      // Quality findings
      auto const it = std::ranges::find(view.assessments, node->id, &audio::NodeQualityAssessment::nodeId);

      if (it != view.assessments.end())
      {
        for (auto const& finding : it->findings)
        {
          auto const findingText = uimodel::playback::audioFindingLabel(finding);

          if (findingText.empty())
          {
            continue;
          }

          auto* findingBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
          constexpr int kFindingSpacing = 6;
          findingBox->set_spacing(kFindingSpacing);
          findingBox->set_margin_start(16);
          findingBox->set_valign(Gtk::Align::CENTER);

          auto* dot = Gtk::make_managed<Gtk::Image>();
          dot->set_from_icon_name("media-record-symbolic");
          constexpr int kDotPixelSize = 10;
          dot->set_pixel_size(kDotPixelSize);

          auto const findingQuality = uimodel::playback::qualityForFinding(finding);

          if (auto const* const cssClass = qualityCssClass(findingQuality); cssClass[0] != '\0')
          {
            dot->add_css_class(cssClass);
          }

          findingBox->append(*dot);

          auto* findingLabel = Gtk::make_managed<Gtk::Label>(findingText);
          findingLabel->add_css_class("dim-label");
          findingBox->append(*findingLabel);

          row->append(*findingBox);
        }
      }

      append(*row);
    }

    // Conclusion separator
    auto const conclusionText = uimodel::playback::audioQualityConclusion(view.quality);

    if (!conclusionText.empty())
    {
      auto* separator = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
      separator->set_margin_top(4);
      separator->set_margin_bottom(4);
      append(*separator);

      auto* conclusionBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      constexpr int kConclusionSpacing = 6;
      conclusionBox->set_spacing(kConclusionSpacing);
      conclusionBox->set_valign(Gtk::Align::CENTER);

      auto* dot = Gtk::make_managed<Gtk::Image>();
      dot->set_from_icon_name("media-record-symbolic");
      constexpr int kConclusionDotSize = 12;
      dot->set_pixel_size(kConclusionDotSize);

      if (auto const* const cssClass = qualityCssClass(view.quality); cssClass[0] != '\0')
      {
        dot->add_css_class(cssClass);
      }

      conclusionBox->append(*dot);

      auto* label = Gtk::make_managed<Gtk::Label>(conclusionText);
      conclusionBox->append(*label);

      append(*conclusionBox);
    }
  }
} // namespace ao::gtk
