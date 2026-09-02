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
#include <string>
#include <string_view>

namespace ao::rt
{
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

  rt::TrackPresentationSpec recommendListPresentation(ListPresentationContext const& context,
                                                      std::span<rt::TrackPresentationPreset const> builtinPresets,
                                                      std::span<rt::CustomTrackPresentationPreset const> customPresets);

  class ListPresentations final
  {
  public:
    using Snapshot = std::map<ListId, std::string>;

    ListPresentations(TrackPresentationCatalog& catalog, rt::LibraryChanges const& changes);
    ~ListPresentations();

    ListPresentations(ListPresentations const&) = delete;
    ListPresentations& operator=(ListPresentations const&) = delete;
    ListPresentations(ListPresentations&&) = delete;
    ListPresentations& operator=(ListPresentations&&) = delete;

    Snapshot snapshot() const { return _presentations; }
    /**
     * Installs a persisted snapshot, dropping every entry whose list the
     * library no longer has. @p knownListIds enumerates the live lists; a
     * virtual id (rt::isVirtualListId) is kept without appearing there.
     *
     * LibraryChanges retires an entry when its list is deleted while this owner
     * is alive, so within a session the map cannot outlive its lists. A
     * snapshot read back from disk carries no such guarantee: it can name a
     * list deleted while the frontend was down, or one whose cleanup write
     * never reached the file. Restoring performs the same removal against the
     * live library so a reused ListId cannot inherit the stale entry.
     */
    void restore(Snapshot presentations, std::span<ListId const> knownListIds);

    /**
     * Returns a borrowed view of the matching string owned by _presentations.
     *
     * The view remains valid until this owner is destroyed or that map entry is
     * replaced or erased. An effective restore(), same-list replacement or
     * clear, library reset, or deletion of that List invalidates it; mutation
     * of a different map entry does not.
     *
     * ListPresentations supplies no synchronization or intrinsic thread
     * affinity, so callers must serialize access to the mutable model. Copy the
     * id before an invalidating operation, reentrant callback, coroutine
     * suspension, or longer-lived storage unless owner lifetime and entry
     * stability remain guaranteed throughout.
     */
    std::optional<std::string_view> presentationIdForList(ListId listId) const;
    void setPresentationIdForList(ListId listId, std::string_view presentationId);
    void clearPresentationForList(ListId listId);

    rt::TrackPresentationSpec presentationForList(ListPresentationContext const& context) const;

    async::Signal<ListId>& signalChanged() noexcept { return *_changedPtr; }

  private:
    TrackPresentationCatalog& _catalog;
    Snapshot _presentations{};
    std::shared_ptr<async::Signal<ListId>> _changedPtr{std::make_shared<async::Signal<ListId>>()};
    async::Subscription _changesSubscription;
  };
} // namespace ao::uimodel
