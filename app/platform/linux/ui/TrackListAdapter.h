// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/expr/Parser.h>
#include <rs/expr/PlanEvaluator.h>
#include <rs/library/MusicLibrary.h>

#include "platform/linux/ui/TrackRow.h"
#include "platform/linux/ui/TrackRowDataProvider.h"
#include <rs/model/TrackIdList.h>

#include <giomm/liststore.h>

#include <memory>
#include <optional>
#include <string>

namespace app::ui
{

  class TrackListAdapter final : public rs::model::TrackIdListObserver
  {
  public:
    using TrackId = rs::TrackId;

    explicit TrackListAdapter(rs::model::TrackIdList& source, TrackRowDataProvider const& provider);
    ~TrackListAdapter() override;

    Glib::RefPtr<Gio::ListModel> getModel() { return _listModel; }

    // Set filter text - filters by common display metadata containing the text.
    void setFilter(Glib::ustring const& filterText);

    // Observer overrides
    void onReset() override;
    void onInserted(TrackId id, std::size_t index) override;
    void onUpdated(TrackId id, std::size_t index) override;
    void onRemoved(TrackId id, std::size_t index) override;

  private:
    void rebuildView();
    void createRowForTrack(TrackId id);

    rs::model::TrackIdList& _source;
    TrackRowDataProvider const& _provider;
    Glib::RefPtr<Gio::ListStore<TrackRow>> _listModel;
    Glib::ustring _filterText;
  };

} // namespace app::ui
