// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AudioQualityCss.h"

#include <ao/audio/Quality.h>
#include <ao/uimodel/playback/quality/AudioQualityFormatter.h>

#include <gtkmm/widget.h>

namespace ao::gtk
{
  char const* qualityCssClass(uimodel::AudioQualityCategory category) noexcept
  {
    using Category = uimodel::AudioQualityCategory;

    switch (category)
    {
      case Category::Medal: return "ao-quality-medal";
      case Category::Positive: return "ao-quality-positive";
      case Category::Diagnostic: return "ao-quality-diagnostic";
      case Category::Warning: return "ao-quality-warning";
      case Category::Informational: return "ao-quality-informational";
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
    for (auto const& cls : {"ao-quality-medal",
                            "ao-quality-positive",
                            "ao-quality-diagnostic",
                            "ao-quality-warning",
                            "ao-quality-informational",
                            "ao-quality-clipped"})
    {
      widget.remove_css_class(cls);
    }
  }
} // namespace ao::gtk
