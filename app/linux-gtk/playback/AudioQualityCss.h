// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>
#include <ao/uimodel/playback/NowPlayingViewModel.h>

#include <gtkmm/widget.h>

namespace ao::gtk
{
  char const* qualityCssClass(uimodel::playback::AudioQualityCategory category) noexcept;
  char const* qualityCssClass(audio::Quality quality) noexcept;
  void clearQualityCssClasses(Gtk::Widget& widget);
} // namespace ao::gtk
