// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <giomm/memoryinputstream.h>
#include <gtkmm.h>

#include <cstdint>
#include <span>

namespace ao::gtk
{
  class CoverArtWidget final : public Gtk::Picture
  {
  public:
    CoverArtWidget();
    ~CoverArtWidget() override;

    // Set cover art from raw image bytes
    void setCoverFromBytes(std::span<std::byte const> bytes);

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

  inline void CoverArtWidget::setCoverFromBytes(std::span<std::byte const> bytes)
  {
    if (bytes.empty())
    {
      clearCover();
      return;
    }

    try
    {
      auto memStream = Gio::MemoryInputStream::create();
      memStream->add_data(bytes.data(), bytes.size(), nullptr);
      auto pixbuf = Gdk::Pixbuf::create_from_stream(memStream);
      set_pixbuf(pixbuf);
    }
    catch (Glib::Error const&)
    {
      clearCover();
    }
  }

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
} // namespace ao::gtk
