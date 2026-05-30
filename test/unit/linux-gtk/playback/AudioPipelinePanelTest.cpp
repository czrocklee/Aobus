// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AudioPipelinePanel.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/Backend.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/widget.h>

#include <cstdint>
#include <string>

namespace ao::gtk
{
  class AudioPipelinePanelTestPeer
  {
  public:
    explicit AudioPipelinePanelTestPeer(AudioPipelinePanel& widget)
      : _widget{widget}
    {
    }

    Gtk::Widget* getFirstChild() const { return _widget.get_first_child(); }

    std::int32_t getChildCount() const
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

  private:
    AudioPipelinePanel& _widget;
  };
} // namespace ao::gtk

namespace ao::gtk::test
{
  TEST_CASE("AudioPipelinePanel - Empty State", "[linux-gtk][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto widget = AudioPipelinePanel{};
    auto peer = AudioPipelinePanelTestPeer{widget};

    auto view = uimodel::playback::AudioPipelineView{};
    widget.apply(view);

    CHECK(peer.getChildCount() == 0);
  }

  TEST_CASE("AudioPipelinePanel - Pipeline Render", "[linux-gtk][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto widget = AudioPipelinePanel{};
    auto peer = AudioPipelinePanelTestPeer{widget};

    auto view = uimodel::playback::AudioPipelineView{};
    view.flow.nodes.push_back({.id = "ao-decoder", .name = "Decoder"});
    view.quality = audio::Quality::BitwisePerfect;

    widget.apply(view);

    // Title + 1 Node + Separator + Conclusion
    CHECK(peer.getChildCount() == 4);
  }

  TEST_CASE("AudioPipelinePanel - Variant Styling", "[linux-gtk][playback]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto widget = AudioPipelinePanel{AudioPipelinePanelVariant::Inline};
    auto peer = AudioPipelinePanelTestPeer{widget};

    CHECK(peer.hasCssClass("ao-quality-panel"));
    CHECK(peer.hasCssClass("ao-quality-panel-inline"));

    widget.setVariant(AudioPipelinePanelVariant::Tooltip);
    CHECK_FALSE(peer.hasCssClass("ao-quality-panel-inline"));
    CHECK(peer.hasCssClass("ao-quality-panel-tooltip"));

    widget.setVariant(AudioPipelinePanelVariant::Compact);
    CHECK_FALSE(peer.hasCssClass("ao-quality-panel-tooltip"));
    CHECK(peer.hasCssClass("ao-quality-panel-compact"));
  }
} // namespace ao::gtk::test
