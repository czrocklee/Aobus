// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AudioQualityCss.h"

#include <ao/audio/Backend.h>
#include <ao/uimodel/playback/now-playing/NowPlayingViewModel.h>

#include <gtkmm/widget.h>

namespace ao::gtk
{
  char const* qualityCssClass(uimodel::AudioQualityCategory category) noexcept
  {
    using Category = uimodel::AudioQualityCategory;

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

  char const* qualityCssClass(audio::Quality quality) noexcept
  {
    return qualityCssClass(uimodel::audioQualityCategory(quality));
  }

  void clearQualityCssClasses(Gtk::Widget& widget)
  {
    for (auto const& cls : {"ao-quality-perfect",
                            "ao-quality-lossless",
                            "ao-quality-intervention",
                            "ao-quality-lossy",
                            "ao-quality-clipped"})
    {
      widget.remove_css_class(cls);
    }
  }
} // namespace ao::gtk
