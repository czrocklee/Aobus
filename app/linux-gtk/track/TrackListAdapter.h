// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/MusicLibrary.h>

#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include "track/TrackRowObject.h"
#include <runtime/ProjectionTypes.h>
#include <runtime/TrackSource.h>

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

  struct ResolvedTrackFilter final
  {
    TrackFilterMode mode = TrackFilterMode::None;
    std::string expression{};
  };

  class TrackListAdapter final : public rt::TrackSourceObserver
  {
  public:
    explicit TrackListAdapter(rt::TrackSource& source,
                              library::MusicLibrary& musicLibrary,
                              TrackRowCache const& provider);
    ~TrackListAdapter() override;

    Glib::RefPtr<Gio::ListModel> getModel() { return _listModel; }
    library::MusicLibrary& getMusicLibrary() { return _musicLibrary; }

    // O(1) index lookup via projection when bound, fallback to source scan.
    std::optional<std::size_t> indexOf(TrackId trackId) const;

    // Bind to a runtime projection for delta-based updates.
    // When bound, the adapter ignores direct TrackIdListObserver callbacks
    // and instead rebuilds the list store from projection deltas.
    void bindProjection(std::shared_ptr<rt::ITrackListProjection> projection);

    // Access the bound projection for group/presentation queries.
    rt::ITrackListProjection* projection() const { return _projection.get(); }
    std::optional<std::size_t> groupIndexForTrack(TrackId trackId) const;

    // Signal emitted when the underlying ListModel is swapped (e.g. during projection binding).
    sigc::signal<void()>& signalModelChanged() { return _signalModelChanged; }

    // Resolve raw filter text to a filter mode and expression.
    static ResolvedTrackFilter resolveFilterExpression(std::string_view rawFilter);

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

    // Batch projection delta application
    void applyDeltaBatch(rt::TrackListProjectionDeltaBatch const& batch);
    void applyResetDelta();
    void applyInsertRange(rt::ProjectionInsertRange const& delta);
    void applyRemoveRange(rt::ProjectionRemoveRange const& delta);
    void applyUpdateRange(rt::ProjectionUpdateRange const& delta);

    rt::TrackSource& _source;
    library::MusicLibrary& _musicLibrary;
    TrackRowCache const& _provider;
    Glib::RefPtr<Gio::ListStore<TrackRowObject>> _listStore;
    Glib::RefPtr<ProjectionTrackModel> _projectionModel;
    Glib::RefPtr<Gio::ListModel> _listModel;
    std::size_t _modelSize = 0;
    sigc::connection _rebuildConnection;

    // Projection binding
    std::shared_ptr<rt::ITrackListProjection> _projection;
    rt::Subscription _projectionSub;
    bool _sourceDetachedForProjection = false;
    sigc::signal<void()> _signalModelChanged;
  };
} // namespace ao::gtk
