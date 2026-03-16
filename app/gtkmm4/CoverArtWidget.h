// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <gtkmm.h>

class CoverArtWidget : public Gtk::Picture
{
public:
  CoverArtWidget();
  ~CoverArtWidget() override;

  void setCoverPixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf);
  void clearCover();
};

// Inline implementation
inline CoverArtWidget::CoverArtWidget()
{
  set_keep_aspect_ratio(true);
  set_alternative_text("No cover art");
}

inline CoverArtWidget::~CoverArtWidget() = default;

inline void CoverArtWidget::setCoverPixbuf(Glib::RefPtr<Gdk::Pixbuf> const& pixbuf)
{
  if (pixbuf)
  {
    set_pixbuf(pixbuf);
  }
  else
  {
    clearCover();
  }
}

inline void CoverArtWidget::clearCover()
{
  // Clear to empty - GTK4 will show placeholder
  set_pixbuf({});
}
