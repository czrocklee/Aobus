// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AudioPipelinePanel.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/Format.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/flow/Graph.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk
{
  class AudioPipelinePanelInspector
  {
  public:
    explicit AudioPipelinePanelInspector(AudioPipelinePanel& widget)
      : _widget{widget}
    {
    }

    std::int32_t childCount() const
    {
      std::int32_t count = 0;
      auto* child = _widget.get_first_child();

      while (child != nullptr)
      {
        count++;
        child = child->get_next_sibling();
      }

      return count;
    }

    bool hasCssClass(std::string const& className) const { return _widget.has_css_class(className); }

    std::vector<std::string> labelTexts() const
    {
      auto labels = std::vector<std::string>{};
      collectLabelTexts(_widget, labels);
      return labels;
    }

    bool hasDescendantCssClass(std::string const& className) const { return containsCssClass(_widget, className); }

  private:
    static void collectLabelTexts(Gtk::Widget& widget, std::vector<std::string>& labels)
    {
      if (auto* const label = dynamic_cast<Gtk::Label*>(&widget); label != nullptr)
      {
        labels.push_back(label->get_text().raw());
      }

      for (auto* child = widget.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        collectLabelTexts(*child, labels);
      }
    }

    static bool containsCssClass(Gtk::Widget& widget, std::string const& className)
    {
      if (widget.has_css_class(className))
      {
        return true;
      }

      for (auto* child = widget.get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        if (containsCssClass(*child, className))
        {
          return true;
        }
      }

      return false;
    }

    AudioPipelinePanel& _widget;
  };
} // namespace ao::gtk

namespace ao::gtk::test
{
  namespace
  {
    bool hasLabel(std::vector<std::string> const& labels, std::string_view const expected)
    {
      return std::ranges::contains(labels, expected);
    }
  } // namespace

  TEST_CASE("AudioPipelinePanel - renders no rows for an empty pipeline", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto widget = AudioPipelinePanel{};
    auto inspector = AudioPipelinePanelInspector{widget};

    auto view = uimodel::AudioPipelineViewState{};
    widget.apply(view);

    CHECK(inspector.childCount() == 0);
  }

  TEST_CASE("AudioPipelinePanel - renders pipeline nodes with header and conclusion", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto widget = AudioPipelinePanel{};
    auto inspector = AudioPipelinePanelInspector{widget};

    auto view = uimodel::AudioPipelineViewState{};
    view.quality = rt::QualityState{
      .sourceQuality = audio::Quality::BitwisePerfect,
      .pipelineQuality = audio::Quality::BitwisePerfect,
      .overall = audio::Quality::BitwisePerfect,
      .assessments = {
        audio::NodeQualityAssessment{
          .nodeId = "ao-source",
          .nodeName = "Source",
          .nodeType = audio::flow::NodeType::Source,
          .optFormat = audio::Format{.sampleRate = 44100, .channels = 2, .bitDepth = 16, .validBits = 16},
          .findings = {audio::QualityFinding{
            .kind = audio::QualityFindingKind::BitPerfect, .quality = audio::Quality::BitwisePerfect}},
        },
      }};

    widget.apply(view);

    auto const labels = inspector.labelTexts();
    CHECK(hasLabel(labels, "Audio Pipeline"));
    CHECK(hasLabel(labels, "[Source]"));
    CHECK(hasLabel(labels, "Source"));
    CHECK(hasLabel(labels, "(44.1 kHz · 16-bit · Stereo)"));
    CHECK(hasLabel(labels, "Bit-perfect playback"));
    CHECK(inspector.hasDescendantCssClass("ao-quality-medal"));
  }

  TEST_CASE("AudioPipelinePanel - replaces variant CSS classes", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto widget = AudioPipelinePanel{AudioPipelinePanelVariant::Inline};
    auto inspector = AudioPipelinePanelInspector{widget};

    CHECK(inspector.hasCssClass("ao-quality-panel"));
    CHECK(inspector.hasCssClass("ao-quality-panel-inline"));

    widget.setVariant(AudioPipelinePanelVariant::Tooltip);
    CHECK_FALSE(inspector.hasCssClass("ao-quality-panel-inline"));
    CHECK(inspector.hasCssClass("ao-quality-panel-tooltip"));

    widget.setVariant(AudioPipelinePanelVariant::Compact);
    CHECK_FALSE(inspector.hasCssClass("ao-quality-panel-tooltip"));
    CHECK(inspector.hasCssClass("ao-quality-panel-compact"));
  }
} // namespace ao::gtk::test
