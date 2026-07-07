// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AudioPipelinePanel.h"

#include "playback/AudioQualityCss.h"
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <gtkmm/separator.h>

#include <format>

namespace ao::gtk
{
  namespace
  {
    constexpr int kPanelSpacing = 6;
  }

  AudioPipelinePanel::AudioPipelinePanel(AudioPipelinePanelVariant variant)
    : Gtk::Box{Gtk::Orientation::VERTICAL}, _variant{variant}
  {
    set_spacing(kPanelSpacing);
    add_css_class("ao-quality-panel");

    setVariant(variant);
  }

  void AudioPipelinePanel::setVariant(AudioPipelinePanelVariant variant)
  {
    remove_css_class("ao-quality-panel-inline");
    remove_css_class("ao-quality-panel-compact");
    remove_css_class("ao-quality-panel-tooltip");

    _variant = variant;

    switch (_variant)
    {
      case AudioPipelinePanelVariant::Inline: add_css_class("ao-quality-panel-inline"); break;
      case AudioPipelinePanelVariant::Compact: add_css_class("ao-quality-panel-compact"); break;
      case AudioPipelinePanelVariant::Tooltip: add_css_class("ao-quality-panel-tooltip"); break;
    }
  }

  void AudioPipelinePanel::apply(uimodel::AudioPipelineView const& view)
  {
    // Remove all existing children
    while (auto* child = get_first_child())
    {
      remove(*child);
    }

    if (view.quality.assessments.empty())
    {
      if (_variant == AudioPipelinePanelVariant::Tooltip)
      {
        set_visible(false);
      }

      return;
    }

    if (_variant == AudioPipelinePanelVariant::Tooltip)
    {
      set_visible(true);
    }

    // Title / Header
    auto* headerBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    headerBox->set_spacing(kPanelSpacing);
    headerBox->set_margin_bottom(4);

    auto const iconName = view.deviceIconName.empty() ? "audio-card-symbolic" : view.deviceIconName;
    auto* icon = Gtk::make_managed<Gtk::Image>();
    icon->set_from_icon_name(iconName);
    icon->set_pixel_size(16);
    headerBox->append(*icon);

    auto const title = view.deviceName.empty() ? "Audio Pipeline" : view.deviceName;
    auto* titleLabel = Gtk::make_managed<Gtk::Label>();
    titleLabel->set_markup(std::format("<b>{}</b>", title));
    titleLabel->set_halign(Gtk::Align::START);
    titleLabel->add_css_class("ao-pipeline-title");
    headerBox->append(*titleLabel);

    append(*headerBox);

    // Separator after title
    auto* topSeparator = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
    topSeparator->set_margin_bottom(2);
    append(*topSeparator);

    for (auto const& assessment : view.quality.assessments)
    {
      auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
      row->set_spacing(2);

      // Node header
      auto* headerBox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
      headerBox->set_spacing(4);

      auto const typeLabel = uimodel::audioNodeTypeLabel(assessment.nodeType);
      auto* typeWidget = Gtk::make_managed<Gtk::Label>(typeLabel);
      typeWidget->add_css_class("dim-label");
      headerBox->append(*typeWidget);

      auto* nameWidget = Gtk::make_managed<Gtk::Label>(assessment.nodeName);
      headerBox->append(*nameWidget);

      // Format details
      if (assessment.optFormat)
      {
        // The source node reports the track's true resolution (valid bits);
        // downstream nodes report the transport container width.
        auto const preferValidBits = assessment.nodeType == audio::flow::NodeType::Source;
        auto const formatStr =
          std::string{"("} + uimodel::audioFormatLabel(*assessment.optFormat, preferValidBits) + ")";
        auto* formatLabel = Gtk::make_managed<Gtk::Label>(formatStr);
        formatLabel->add_css_class("dim-label");
        headerBox->append(*formatLabel);
      }

      row->append(*headerBox);

      // Quality findings
      for (auto const& finding : assessment.findings)
      {
        auto const findingText = uimodel::audioFindingLabel(finding);

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

        if (auto const* const cssClass = qualityCssClass(uimodel::audioFindingCategory(finding)); cssClass[0] != '\0')
        {
          dot->add_css_class(cssClass);
        }

        findingBox->append(*dot);

        auto* findingLabel = Gtk::make_managed<Gtk::Label>(findingText);
        findingLabel->add_css_class("dim-label");
        findingBox->append(*findingLabel);

        row->append(*findingBox);
      }

      append(*row);
    }

    // Conclusion separator
    auto const presentation = uimodel::audioQualityPresentation(view.quality);

    if (!presentation.headline.empty())
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

      if (auto const* const cssClass = qualityCssClass(presentation.category); cssClass[0] != '\0')
      {
        dot->add_css_class(cssClass);
      }

      conclusionBox->append(*dot);

      auto* label = Gtk::make_managed<Gtk::Label>(presentation.headline);
      conclusionBox->append(*label);

      append(*conclusionBox);
    }
  }
} // namespace ao::gtk
