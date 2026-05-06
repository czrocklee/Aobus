// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/MusicLibrary.h>
#include <ao/query/Parser.h>
#include <ao/query/PlanEvaluator.h>

#include "TrackRow.h"
#include "TrackRowDataProvider.h"
#include <ao/model/TrackIdList.h>
#include <runtime/ProjectionTypes.h>

#include <giomm/liststore.h>

#include <memory>
#include <optional>
#include <string>

namespace ao::gtk
{
  enum class TrackFilterMode
  {
    None,
    Quick,
    Expression,
  };

  class TrackListAdapter final : public ao::model::TrackIdListObserver
  {
  public:
    using TrackId = ao::TrackId;

    explicit TrackListAdapter(ao::model::TrackIdList& source,
                              ao::library::MusicLibrary& musicLibrary,
                              TrackRowDataProvider const& provider);
    ~TrackListAdapter() override;

    Glib::RefPtr<Gio::ListModel> getModel() { return _listModel; }
    ao::library::MusicLibrary& getMusicLibrary() { return _musicLibrary; }

    // Bind to a runtime projection for delta-based updates.
    // When bound, the adapter ignores direct TrackIdListObserver callbacks
    // and instead rebuilds the list store from projection deltas.
    void bindProjection(ao::app::ITrackListProjection& projection);

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
    void onUpdated(std::span<TrackId const> ids) override;
    void onInserted(std::span<TrackId const> ids) override;
    void onRemoved(std::span<TrackId const> ids) override;

  private:
    void rebuildView();
    void rebuildViewInternal();
    void createRowForTrack(TrackId id);
    bool shouldIncludeTrack(TrackId id, ao::library::TrackStore::Reader& reader) const;

    ao::model::TrackIdList& _source;
    ao::library::MusicLibrary& _musicLibrary;
    TrackRowDataProvider const& _provider;
    Glib::RefPtr<Gio::ListStore<TrackRow>> _listModel;
    Glib::ustring _filterText;
    TrackFilterMode _filterMode = TrackFilterMode::None;
    std::string _filterExpression;
    std::string _filterErrorMessage;
    std::unique_ptr<ao::query::ExecutionPlan> _filterPlan;
    ao::query::PlanEvaluator _filterEvaluator;
    sigc::connection _rebuildConnection;

    // Projection binding
    ao::app::ITrackListProjection* _projection = nullptr;
    ao::app::Subscription _projectionSub;
  };
} // namespace ao::gtk
