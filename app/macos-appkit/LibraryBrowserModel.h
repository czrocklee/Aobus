// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/ViewIds.h>
#include <ao/uimodel/library/list/ListTreeProjection.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_set>
#include <vector>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::rt
{
  struct TrackRow;
}

namespace ao::uimodel
{
  class TrackDisplayIndex;
  struct TrackGroupHeadingPresentation;
}

namespace ao::appkit
{
  class LibrarySession;

  // Main-thread browser reads and intents. The session owns this model; the browser
  // must detach before the session closes. There is no independently retained work.
  class LibraryBrowserModel final
  {
  public:
    // View ID and revision are sampled; the index is borrowed for synchronous use.
    struct Table final
    {
      rt::ViewId viewId;
      std::uint64_t revision;
      uimodel::TrackDisplayIndex const& index;
    };

    struct Sidebar final
    {
      uimodel::ListTreeProjection tree{};
      std::unordered_set<ListId> writableListIds{};
    };

    ~LibraryBrowserModel() = default;
    LibraryBrowserModel(LibraryBrowserModel const&) = delete;
    LibraryBrowserModel& operator=(LibraryBrowserModel const&) = delete;
    LibraryBrowserModel(LibraryBrowserModel&&) = delete;
    LibraryBrowserModel& operator=(LibraryBrowserModel&&) = delete;

    i18n::MessageCatalog const& catalog() const noexcept;
    // Live focus can change before the table binds to the new projection.
    ListId activeListId() const;
    Table boundTable() const noexcept;
    std::uint64_t listRevision() const noexcept;
    Sidebar projectSidebar() const;
    std::optional<rt::TrackColumnSort> renderedSort(rt::ViewId renderedViewId) const noexcept;

    // Lazy row borrow for immediate use, not valid across table revisions.
    rt::TrackRow const* rowAt(std::size_t displayIndex);
    uimodel::TrackGroupHeadingPresentation groupHeading(std::size_t groupIndex) const;
    std::optional<std::size_t> displayIndexOf(TrackId trackId) const;
    std::vector<TrackId> boundSelection() const;

    void navigateToList(ListId listId) const;
    void selectTracks(std::vector<TrackId> trackIds);
    Result<> toggleColumnSort(rt::ViewId renderedViewId, rt::TrackField column) const;
    void playTrack(TrackId trackId);
    void playSelection();
    void togglePlayback();

  private:
    friend class LibrarySession;
    explicit LibraryBrowserModel(LibrarySession& session) noexcept
      : _session{session}
    {
    }

    LibrarySession& _session;
  };
} // namespace ao::appkit
