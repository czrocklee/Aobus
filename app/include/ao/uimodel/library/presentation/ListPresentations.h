// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/async/Signal.h>
#include <ao/async/Subscription.h>
#include <ao/rt/TrackPresentation.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ao::rt
{
  class Library;
  class LibraryChanges;
}

namespace ao::uimodel
{
  class TrackPresentationCatalog;

  enum class ListPresentationSourceKind : std::uint8_t
  {
    AllTracks,
    SavedList,
  };

  struct ListPresentationContext final
  {
    ListId listId = kInvalidListId;
    ListPresentationSourceKind sourceKind = ListPresentationSourceKind::AllTracks;
    std::string_view listExpression{};
  };

  bool isTrackPresentationEligibleForList(ListId listId, rt::TrackPresetId const& id) noexcept;

  rt::TrackPresentationSpec recommendListPresentation(ListPresentationContext const& context,
                                                      std::span<rt::TrackPresentationPreset const> builtinPresets);

  class ListPresentations final
  {
  public:
    using Snapshot = std::map<ListId, rt::TrackPresetId>;

    ListPresentations(TrackPresentationCatalog& catalog, rt::LibraryChanges const& changes);
    ~ListPresentations();

    ListPresentations(ListPresentations const&) = delete;
    ListPresentations& operator=(ListPresentations const&) = delete;
    ListPresentations(ListPresentations&&) = delete;
    ListPresentations& operator=(ListPresentations&&) = delete;

    Snapshot snapshot() const { return _presentations; }
    /**
     * Installs a persisted snapshot, dropping every entry whose list the
     * library no longer has. @p knownListIds
     * enumerates the live lists; a virtual id (rt::isVirtualListId) is kept
     * without appearing there.
     *
     * LibraryChanges retires an entry when its list is deleted while this owner
     * is alive, so within a session the map cannot outlive its lists. A
     * snapshot read back from disk carries no such guarantee: it can name a
     * list deleted while the frontend was down, or one whose cleanup write
     * never reached the file. Restoring performs the same removal against the
     * live library so a reused ListId cannot inherit the stale entry.
     */
    void restore(Snapshot presentations, std::span<ListId const> knownListIds);

    std::optional<rt::TrackPresetId> presentationIdForList(ListId listId) const;
    void setPresentationIdForList(ListId listId, rt::TrackPresetId presentationId);
    void clearPresentationForList(ListId listId);

    rt::TrackPresentationSpec presentationForList(ListPresentationContext const& context) const;

    /// Recommends from the list's local expression, or as All Tracks for a virtual or missing list.
    /// An available, eligible pin for the requested ID still takes precedence.
    rt::TrackPresentationSpec presentationForList(ListId listId, rt::Library const& library) const;

    async::Signal<ListId>& signalChanged() noexcept { return *_changedPtr; }

  private:
    TrackPresentationCatalog& _catalog;
    Snapshot _presentations{};
    std::shared_ptr<async::Signal<ListId>> _changedPtr{std::make_shared<async::Signal<ListId>>()};
    async::Subscription _changesSubscription;
  };
} // namespace ao::uimodel
