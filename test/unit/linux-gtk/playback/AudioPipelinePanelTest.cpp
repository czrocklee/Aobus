// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AudioPipelinePanel.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkLayoutTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include <ao/audio/ChannelDeliveryEvidence.h>
#include <ao/audio/Quality.h>
#include <ao/audio/QualityAnalyzer.h>
#include <ao/audio/SignalFormat.h>
#include <ao/audio/flow/Graph.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <gtkmm/enums.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    class AudioPipelinePanelInspector final
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
  } // namespace
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
    auto widget = AudioPipelinePanel{ao::test::englishMessageCatalog()};
    auto inspector = AudioPipelinePanelInspector{widget};

    auto view = uimodel::AudioPipelineViewState{};
    widget.apply(view);

    CHECK(inspector.childCount() == 0);
  }

  TEST_CASE("AudioPipelinePanel - renders pipeline nodes with header and conclusion", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto widget = AudioPipelinePanel{ao::test::englishMessageCatalog()};
    auto inspector = AudioPipelinePanelInspector{widget};

    auto view = uimodel::AudioPipelineViewState{};
    view.quality = rt::QualityState{
      .sourceQuality = audio::Quality::BitwisePerfect,
      .pipelineQuality = audio::Quality::BitwisePerfect,
      .overall = audio::Quality::BitwisePerfect,
      .channelConfidence = audio::ChannelConfidence::Verified,
      .assessments = {
        audio::NodeQualityAssessment{
          .nodeId = "ao-source",
          .nodeName = "Source",
          .nodeType = audio::flow::NodeType::Source,
          .optFormat = audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16, .speakerMask = 0x3},
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
    CHECK(hasLabel(labels, "Bit-perfect PCM (observed software path)"));
    CHECK_FALSE(hasLabel(labels, "Channel delivery not proved"));
    CHECK(inspector.hasDescendantCssClass("ao-quality-medal"));

    SECTION("unverified delivery adds a separate note without discarding numerical evidence")
    {
      view.quality.channelConfidence = audio::ChannelConfidence::Unverified;
      widget.apply(view);
      auto const unverifiedLabels = inspector.labelTexts();
      CHECK(hasLabel(unverifiedLabels, "(44.1 kHz · 16-bit · Stereo)"));
      CHECK(hasLabel(unverifiedLabels, "Channel delivery not proved"));
      CHECK(hasLabel(unverifiedLabels, "Bit-perfect PCM (observed software path)"));
      CHECK(inspector.hasDescendantCssClass("ao-quality-medal"));

      view.quality.channelConfidence = audio::ChannelConfidence::Verified;
      widget.apply(view);
      CHECK_FALSE(hasLabel(inspector.labelTexts(), "Channel delivery not proved"));
    }
  }

  TEST_CASE("AudioPipelinePanel - headline and channel note wrap across managed-label rebuilds",
            "[gtk][unit][playback][geometry][localization]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    for (auto const* const locale : {"en-US", "zh-CN"})
    {
      CAPTURE(locale);
      auto const catalog = ao::test::messageCatalog(locale);
      auto const formatter = uimodel::AudioQualityFormatter{catalog};
      auto widget = AudioPipelinePanel{catalog};
      auto inspector = AudioPipelinePanelInspector{widget};
      auto view = uimodel::AudioPipelineViewState{
        .quality = {.sourceQuality = audio::Quality::BitwisePerfect,
                    .pipelineQuality = audio::Quality::BitwisePerfect,
                    .overall = audio::Quality::BitwisePerfect,
                    .assessments = {{.nodeType = audio::flow::NodeType::Source,
                                     .optFormat =
                                       audio::SignalFormat{.sampleRate = 44100, .channels = 2, .precisionBits = 16}}}},
      };
      auto previousNote = std::string{};

      for (auto const confidence : {audio::ChannelConfidence::Unknown,
                                    audio::ChannelConfidence::Verified,
                                    audio::ChannelConfidence::Unverified,
                                    audio::ChannelConfidence::Unknown})
      {
        CAPTURE(confidence);
        view.quality.channelConfidence = confidence;
        auto const presentation = formatter.presentation(view.quality);
        REQUIRE(presentation.category == uimodel::AudioQualityCategory::Medal);
        widget.apply(view);
        auto const labels = inspector.labelTexts();
        CHECK(std::ranges::count(labels, presentation.headline) == 1);

        if (!previousNote.empty() && previousNote != presentation.channelNote)
        {
          CHECK_FALSE(hasLabel(labels, previousNote));
        }

        // Borrow labels only within this apply generation. Re-find them after
        // each rebuild; no pointer or reference crosses the next apply call.
        for (auto const& text : {presentation.headline, presentation.channelNote})
        {
          if (text.empty())
          {
            continue;
          }

          CHECK(std::ranges::count(labels, text) == 1);
          auto* const label = findLabelByText(widget, text);
          REQUIRE(label != nullptr);
          CHECK(label->get_wrap());
          // Respect GTK's horizontal minimum before measuring height-for-width;
          // a smaller budget is invalid, not evidence of wrapping. Keep the
          // constrained budget near 64px and the wide control at least natural
          // width without pinning a font, line count, or pixel height.
          auto const width = measureWidget(*label, Gtk::Orientation::HORIZONTAL);
          auto const narrowColumns = std::max(64, width.minimum);
          auto const wideColumns = std::max(640, width.natural);
          REQUIRE(narrowColumns < wideColumns);
          auto const wide = measureWidget(*label, Gtk::Orientation::VERTICAL, wideColumns);
          auto const narrow = measureWidget(*label, Gtk::Orientation::VERTICAL, narrowColumns);
          CHECK(narrow.natural > wide.natural);
          CHECK(label->get_text().raw() == text);
        }

        previousNote = presentation.channelNote;
      }
    }
  }

  TEST_CASE("AudioPipelinePanel - realized windows keep the wrapped note inside the allocated panel",
            "[gtk][unit][playback][geometry]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto const catalog = ao::test::messageCatalog("en-US");
    auto const formatter = uimodel::AudioQualityFormatter{catalog};
    auto view = uimodel::AudioPipelineViewState{
      .quality = {.sourceQuality = audio::Quality::BitwisePerfect,
                  .pipelineQuality = audio::Quality::BitwisePerfect,
                  .overall = audio::Quality::BitwisePerfect,
                  .assessments = {{.nodeType = audio::flow::NodeType::Source}}},
    };

    struct RealizedNote final
    {
      std::int32_t width = 0;
      std::int32_t height = 0;
    };

    auto normalNote = RealizedNote{};
    auto constrainedNote = RealizedNote{};

    auto const exerciseWindow = [&](bool const constrained, RealizedNote& unknownNote)
    {
      auto widget = AudioPipelinePanel{catalog};
      // Unmount and drain before the widget wrapper dies, even if REQUIRE aborts this pass.
      auto windowFixture = GtkWindowFixture{};
      auto inspector = AudioPipelinePanelInspector{widget};

      view.quality.channelConfidence = audio::ChannelConfidence::Unknown;
      widget.apply(view);
      // The constrained pass requests the panel's own measured horizontal
      // minimum — the valid allocation floor observed from the widget, not an
      // invented budget; the normal pass keeps the wide control size.
      auto const measured = measureWidget(widget, Gtk::Orientation::HORIZONTAL);
      auto const requestedWidth = constrained ? measured.minimum : std::max(640, measured.natural);
      windowFixture.window().set_default_size(requestedWidth, 480);
      windowFixture.mount(widget);
      windowFixture.present();
      REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil(
        [&] { return widget.get_width() >= measured.minimum && widget.get_height() > 0; }));

      auto previousNote = std::string{};

      for (auto const confidence : {audio::ChannelConfidence::Unknown,
                                    audio::ChannelConfidence::Unverified,
                                    audio::ChannelConfidence::Inferred,
                                    audio::ChannelConfidence::Verified})
      {
        CAPTURE(confidence);
        view.quality.channelConfidence = confidence;
        auto const presentation = formatter.presentation(view.quality);
        REQUIRE(presentation.category == uimodel::AudioQualityCategory::Medal);
        widget.apply(view);

        // Borrow labels only within this apply generation; re-find them after
        // each rebuild, as the shipping panel rebuilds its managed children.
        auto const labels = inspector.labelTexts();
        auto* const headlineLabel = findLabelByText(widget, presentation.headline);
        REQUIRE(headlineLabel != nullptr);
        auto* const noteLabel =
          presentation.channelNote.empty() ? nullptr : findLabelByText(widget, presentation.channelNote);
        REQUIRE((presentation.channelNote.empty() || noteLabel != nullptr));
        REQUIRE(ao::gtk::test::tryPumpGtkEventsUntil(
          [&]
          {
            return headlineLabel->get_width() > 0 && headlineLabel->get_height() > 0 &&
                   (noteLabel == nullptr || (noteLabel->get_width() > 0 && noteLabel->get_height() > 0));
          }));
        REQUIRE(headlineLabel->get_realized());
        REQUIRE(headlineLabel->get_mapped());
        CHECK(headlineLabel->get_wrap());
        CHECK(headlineLabel->get_width() <= widget.get_width());
        CHECK(widget.get_width() <= windowFixture.window().get_width());
        CHECK(widget.get_height() <= windowFixture.window().get_height());
        CHECK(inspector.hasDescendantCssClass("ao-quality-medal"));

        if (presentation.channelNote.empty())
        {
          CHECK(confidence == audio::ChannelConfidence::Verified);
          CHECK_FALSE(hasLabel(labels, previousNote));
          continue;
        }

        CHECK(hasLabel(labels, presentation.channelNote));

        if (!previousNote.empty() && previousNote != presentation.channelNote)
        {
          CHECK_FALSE(hasLabel(labels, previousNote));
        }

        REQUIRE(noteLabel != nullptr);
        REQUIRE(noteLabel->get_realized());
        REQUIRE(noteLabel->get_mapped());
        CHECK(noteLabel->get_wrap());
        CHECK(noteLabel->has_css_class("dim-label"));
        CHECK(noteLabel->get_width() <= widget.get_width());

        auto const layoutPtr = noteLabel->get_layout();
        REQUIRE(layoutPtr);
        std::int32_t textWidth = 0;
        std::int32_t textHeight = 0;
        layoutPtr->get_pixel_size(textWidth, textHeight);
        CHECK(noteLabel->get_width() >= textWidth);
        CHECK(noteLabel->get_height() >= textHeight);

        if (confidence == audio::ChannelConfidence::Unknown)
        {
          unknownNote = RealizedNote{.width = noteLabel->get_width(), .height = noteLabel->get_height()};

          if (constrained && normalNote.height > 0)
          {
            // The height-for-width curve at the realized widths scopes the
            // final comparison: the note must measure taller at its
            // constrained allocation before realized wrap growth is claimed.
            auto const constrainedCurve = measureWidget(*noteLabel, Gtk::Orientation::VERTICAL, unknownNote.width);
            auto const normalCurve = measureWidget(*noteLabel, Gtk::Orientation::VERTICAL, normalNote.width);
            REQUIRE(normalCurve.natural > 0);
            REQUIRE(constrainedCurve.natural > normalCurve.natural);
          }
        }

        previousNote = presentation.channelNote;
      }

      windowFixture.unmount();
    };

    exerciseWindow(false, normalNote);
    exerciseWindow(true, constrainedNote);

    REQUIRE(normalNote.height > 0);
    REQUIRE(constrainedNote.height > 0);
    CHECK(constrainedNote.height > normalNote.height);
  }

  TEST_CASE("AudioPipelinePanel - replaces variant CSS classes", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto widget = AudioPipelinePanel{ao::test::englishMessageCatalog(), AudioPipelinePanelVariant::Inline};
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

  TEST_CASE("AudioPipelinePanel - renders shared pipeline semantics in the selected locale",
            "[gtk][unit][playback][localization]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto catalog = ao::test::messageCatalog("de-DE");
    auto widget = AudioPipelinePanel{catalog};
    auto inspector = AudioPipelinePanelInspector{widget};
    auto view = uimodel::AudioPipelineViewState{
      .quality =
        rt::QualityState{
          .sourceQuality = audio::Quality::BitwisePerfect,
          .pipelineQuality = audio::Quality::BitwisePerfect,
          .overall = audio::Quality::BitwisePerfect,
          .channelConfidence = audio::ChannelConfidence::Verified,
          .assessments = {audio::NodeQualityAssessment{
            .nodeName = "Dvořák DAC",
            .nodeType = audio::flow::NodeType::Source,
            .findings = {audio::QualityFinding{
              .kind = audio::QualityFindingKind::BitPerfect,
              .quality = audio::Quality::BitwisePerfect,
            }},
          }},
        },
    };

    widget.apply(view);
    auto const labels = inspector.labelTexts();
    CHECK(hasLabel(labels, "Audiokette"));
    CHECK(hasLabel(labels, "[Quelle]"));
    CHECK(hasLabel(labels, "Dvořák DAC"));
    CHECK(hasLabel(labels, "Bitgenaues PCM (beobachteter Softwarepfad)"));
  }

  TEST_CASE("AudioPipelinePanel - escapes external device names before applying markup", "[gtk][unit][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto widget = AudioPipelinePanel{ao::test::englishMessageCatalog()};
    auto inspector = AudioPipelinePanelInspector{widget};
    auto view = uimodel::AudioPipelineViewState{
      .quality =
        rt::QualityState{
          .assessments = {audio::NodeQualityAssessment{.nodeName = "Source"}},
        },
      .deviceName = "Dvořák & Sons <DAC>",
    };

    widget.apply(view);

    CHECK(hasLabel(inspector.labelTexts(), "Dvořák & Sons <DAC>"));
  }
} // namespace ao::gtk::test
