#pragma once

#include <gtkmm.h>

class CoverArtWidget : public Gtk::Picture
{
public:
  CoverArtWidget();
  ~CoverArtWidget() override;

  void setCoverPixbuf(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf);
  void clearCover();
};

// Inline implementation
inline CoverArtWidget::CoverArtWidget()
{
  set_keep_aspect_ratio(true);
  set_alternative_text("No cover art");
}

inline CoverArtWidget::~CoverArtWidget() = default;

inline void CoverArtWidget::setCoverPixbuf(const Glib::RefPtr<Gdk::Pixbuf>& pixbuf)
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
