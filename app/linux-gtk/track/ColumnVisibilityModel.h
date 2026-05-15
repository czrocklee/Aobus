// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "track/TrackPresentation.h"

#include <gtkmm.h>

#include <unordered_set>

namespace ao::gtk
{
  class ColumnVisibilityModel final : public Glib::Object
  {
  public:
    static Glib::RefPtr<ColumnVisibilityModel> create();

    Glib::PropertyProxy<bool> property_visible(TrackColumn column);

    bool isVisible(TrackColumn column) const noexcept;

    void recompute(TrackColumnLayout const& layout);
    void setAllFalse();

  protected:
    ColumnVisibilityModel();

  private:
    Glib::Property<bool> _propArtist;
    Glib::Property<bool> _propAlbum;
    Glib::Property<bool> _propAlbumArtist;
    Glib::Property<bool> _propGenre;
    Glib::Property<bool> _propComposer;
    Glib::Property<bool> _propWork;
    Glib::Property<bool> _propYear;
    Glib::Property<bool> _propDiscNumber;
    Glib::Property<bool> _propTrackNumber;
    Glib::Property<bool> _propTitle;
    Glib::Property<bool> _propDuration;
    Glib::Property<bool> _propTags;
  };
} // namespace ao::gtk
