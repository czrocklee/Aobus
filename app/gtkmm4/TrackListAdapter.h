// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/MusicLibrary.h>
#include <rs/expr/Evaluator.h>
#include <rs/expr/Parser.h>
#include <rs/fbs/Track_generated.h>
#include <rs/reactive/AbstractItemList.h>

#include <giomm/liststore.h>

#include <optional>
#include <string>

#include "TrackRow.h"

class TrackListAdapter
  : public rs::reactive::AbstractItemList<rs::core::MusicLibrary::TrackId, rs::fbs::TrackT>::Observer
{
public:
  using TrackId = rs::core::MusicLibrary::TrackId;
  using AbstractTrackList = rs::reactive::AbstractItemList<TrackId, rs::fbs::TrackT>;

  explicit TrackListAdapter(AbstractTrackList& tracks);
  ~TrackListAdapter() override;

  Glib::RefPtr<Gio::ListModel> getModel() { return _listModel; }

  // Set filter text - filters by artist, album, or title containing the text
  void setFilter(Glib::ustring const& filterText);

  // Set expression filter - filters based on expression
  void setExprFilter(std::string const& exprString);

  // Observer overrides
  void onAttached() override;
  void onBeginInsert(TrackId id, AbstractTrackList::Index index) override;
  void onEndInsert(TrackId id, rs::fbs::TrackT const& track, AbstractTrackList::Index index) override;
  void onBeginUpdate(TrackId id, rs::fbs::TrackT const& track, AbstractTrackList::Index index) override;
  void onEndUpdate(TrackId id, rs::fbs::TrackT const& track, AbstractTrackList::Index index) override;
  void onBeginRemove(TrackId id, rs::fbs::TrackT const& track, AbstractTrackList::Index index) override;
  void onEndRemove(TrackId id, AbstractTrackList::Index index) override;
  void onBeginClear() override;
  void onEndClear() override;
  void onDetached() override;

private:
  void refreshFilteredView();

  AbstractTrackList& _tracks;
  Glib::RefPtr<Gio::ListStore<TrackRow>> _listModel;
  Glib::ustring _filterText;
  std::optional<rs::expr::Expression> _exprFilter;
};
