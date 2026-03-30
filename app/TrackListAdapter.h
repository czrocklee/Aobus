// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/MusicLibrary.h>
#include <rs/expr/Parser.h>
#include <rs/expr/PlanEvaluator.h>

#include "TrackRow.h"
#include "model/TrackIdList.h"
#include "model/TrackRowDataProvider.h"

#include <giomm/liststore.h>

#include <memory>
#include <optional>
#include <string>

class TrackListAdapter final : public app::model::TrackIdListObserver
{
public:
  using TrackId = rs::core::TrackId;

  explicit TrackListAdapter(app::model::TrackIdList& source,
                            std::shared_ptr<app::model::TrackRowDataProvider> provider);
  ~TrackListAdapter() override;

  Glib::RefPtr<Gio::ListModel> getModel() { return _listModel; }

  // Set filter text - filters by artist, album, or title containing the text
  void setFilter(Glib::ustring const& filterText);

  // Observer overrides
  void onReset() override;
  void onInserted(TrackId id, std::size_t index) override;
  void onUpdated(TrackId id, std::size_t index) override;
  void onRemoved(TrackId id, std::size_t index) override;

private:
  void rebuildView();
  void createRowForTrack(TrackId id);

  app::model::TrackIdList* _source;
  std::shared_ptr<app::model::TrackRowDataProvider> _provider;
  Glib::RefPtr<Gio::ListStore<TrackRow>> _listModel;
  Glib::ustring _filterText;
};