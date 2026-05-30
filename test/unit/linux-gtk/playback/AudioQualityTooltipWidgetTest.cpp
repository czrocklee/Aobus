// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AudioQualityTooltipWidget.h"

#include <ao/audio/Backend.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/widget.h>

#include <cstdint>

namespace ao::gtk
{
  class AudioQualityTooltipWidgetTestPeer
  {
  public:
    explicit AudioQualityTooltipWidgetTestPeer(AudioQualityTooltipWidget& widget)
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

  private:
    AudioQualityTooltipWidget& _widget;
  };
} // namespace ao::gtk

namespace ao::gtk::test
{
  TEST_CASE("AudioQualityTooltipWidget - Empty State", "[linux-gtk][playback]")
  {
    auto widget = AudioQualityTooltipWidget{};
    auto peer = AudioQualityTooltipWidgetTestPeer{widget};

    auto view = uimodel::playback::AudioQualityTooltipView{};
    widget.apply(view);

    CHECK(peer.getChildCount() == 0);
  }

  TEST_CASE("AudioQualityTooltipWidget - Pipeline Render", "[linux-gtk][playback]")
  {
    auto widget = AudioQualityTooltipWidget{};
    auto peer = AudioQualityTooltipWidgetTestPeer{widget};

    auto view = uimodel::playback::AudioQualityTooltipView{};
    view.flow.nodes.push_back({.id = "ao-decoder", .name = "Decoder"});
    view.quality = audio::Quality::BitwisePerfect;

    widget.apply(view);

    // Title + 1 Node + Separator + Conclusion
    CHECK(peer.getChildCount() == 4);
  }
} // namespace ao::gtk::test
