// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/ColumnVisibilityModel.h"
#include "track/TrackPresentation.h"

#include <glibmm/objectbase.h>
#include <glibmm/propertyproxy.h>
#include <glibmm/refptr.h>

namespace ao::gtk
{
  ColumnVisibilityModel::ColumnVisibilityModel()
    : Glib::ObjectBase{"ColumnVisibilityModel"}
    , _propArtist{*this, "artist-visible", true}
    , _propAlbum{*this, "album-visible", true}
    , _propAlbumArtist{*this, "album-artist-visible", false}
    , _propGenre{*this, "genre-visible", false}
    , _propComposer{*this, "composer-visible", false}
    , _propWork{*this, "work-visible", false}
    , _propYear{*this, "year-visible", false}
    , _propDiscNumber{*this, "disc-number-visible", false}
    , _propTrackNumber{*this, "track-number-visible", true}
    , _propTitle{*this, "title-visible", true}
    , _propDuration{*this, "duration-visible", true}
    , _propTags{*this, "tags-visible", true}
  {
  }

  Glib::RefPtr<ColumnVisibilityModel> ColumnVisibilityModel::create()
  {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    return Glib::make_refptr_for_instance<ColumnVisibilityModel>(new ColumnVisibilityModel());
  }

  Glib::PropertyProxy<bool> ColumnVisibilityModel::property_visible(TrackColumn column)
  {
    switch (column)
    {
      case TrackColumn::Artist: return _propArtist.get_proxy();
      case TrackColumn::Album: return _propAlbum.get_proxy();
      case TrackColumn::AlbumArtist: return _propAlbumArtist.get_proxy();
      case TrackColumn::Genre: return _propGenre.get_proxy();
      case TrackColumn::Composer: return _propComposer.get_proxy();
      case TrackColumn::Work: return _propWork.get_proxy();
      case TrackColumn::Year: return _propYear.get_proxy();
      case TrackColumn::DiscNumber: return _propDiscNumber.get_proxy();
      case TrackColumn::TrackNumber: return _propTrackNumber.get_proxy();
      case TrackColumn::Title: return _propTitle.get_proxy();
      case TrackColumn::Duration: return _propDuration.get_proxy();
      case TrackColumn::Tags: return _propTags.get_proxy();
    }

    return _propTitle.get_proxy();
  }

  bool ColumnVisibilityModel::isVisible(TrackColumn column) const noexcept
  {
    switch (column)
    {
      case TrackColumn::Artist: return _propArtist.get_value();
      case TrackColumn::Album: return _propAlbum.get_value();
      case TrackColumn::AlbumArtist: return _propAlbumArtist.get_value();
      case TrackColumn::Genre: return _propGenre.get_value();
      case TrackColumn::Composer: return _propComposer.get_value();
      case TrackColumn::Work: return _propWork.get_value();
      case TrackColumn::Year: return _propYear.get_value();
      case TrackColumn::DiscNumber: return _propDiscNumber.get_value();
      case TrackColumn::TrackNumber: return _propTrackNumber.get_value();
      case TrackColumn::Title: return _propTitle.get_value();
      case TrackColumn::Duration: return _propDuration.get_value();
      case TrackColumn::Tags: return _propTags.get_value();
    }

    return false;
  }

  void ColumnVisibilityModel::recompute(TrackColumnLayout const& layout)
  {
    for (auto const& state : layout.columns)
    {
      auto proxy = property_visible(state.column);

      if (proxy.get_value() != state.visible)
      {
        proxy.set_value(state.visible);
      }
    }
  }

  void ColumnVisibilityModel::setAllFalse()
  {
    _propArtist.set_value(false);
    _propAlbum.set_value(false);
    _propAlbumArtist.set_value(false);
    _propGenre.set_value(false);
    _propComposer.set_value(false);
    _propWork.set_value(false);
    _propYear.set_value(false);
    _propDiscNumber.set_value(false);
    _propTrackNumber.set_value(false);
    _propTitle.set_value(false);
    _propDuration.set_value(false);
    _propTags.set_value(false);
  }
} // namespace ao::gtk
