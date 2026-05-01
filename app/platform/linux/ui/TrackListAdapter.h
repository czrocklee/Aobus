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

  enum class TrackFilterMode
  {
    None,
    Quick,
    Expression,
  };

  class TrackListAdapter final : public rs::model::TrackIdListObserver
  {
  public:
    using TrackId = rs::TrackId;

    explicit TrackListAdapter(rs::model::TrackIdList& source,
                              rs::library::MusicLibrary& musicLibrary,
                              TrackRowDataProvider const& provider);
    ~TrackListAdapter() override;

    Glib::RefPtr<Gio::ListModel> getModel() { return _listModel; }

    // Set filter text - filters by common display metadata containing the text.
    void setFilter(Glib::ustring const& filterText);
    TrackFilterMode filterMode() const { return _filterMode; }
    bool hasFilterError() const { return !_filterErrorMessage.empty(); }
    std::string const& filterErrorMessage() const { return _filterErrorMessage; }
    std::string const& currentSmartFilterExpression() const { return _filterExpression; }

    // Observer overrides
    void onReset() override;
    void onInserted(TrackId id, std::size_t index) override;
    void onUpdated(TrackId id, std::size_t index) override;
    void onRemoved(TrackId id, std::size_t index) override;

  private:
    void rebuildView();
    void createRowForTrack(TrackId id);
    bool shouldIncludeTrack(TrackId id, rs::library::TrackStore::Reader& reader) const;

    rs::model::TrackIdList& _source;
    rs::library::MusicLibrary& _musicLibrary;
    TrackRowDataProvider const& _provider;
    Glib::RefPtr<Gio::ListStore<TrackRow>> _listModel;
    Glib::ustring _filterText;
    TrackFilterMode _filterMode = TrackFilterMode::None;
    std::string _filterExpression;
    std::string _filterErrorMessage;
    std::unique_ptr<rs::expr::ExecutionPlan> _filterPlan;
    rs::expr::PlanEvaluator _filterEvaluator;
  };

} // namespace app::ui
