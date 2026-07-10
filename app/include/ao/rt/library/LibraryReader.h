// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/rt/ListNode.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>
#include <ao/rt/TrackRow.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  class Library;

  // A scoped read cursor over the music library. Owns one read transaction for
  // its lifetime; all read accessors execute under that single transaction, so
  // a batch of reads (e.g. many fields across many tracks) stays consistent and
  // pays for only one transaction. Obtain one via Library::reader().
  class [[nodiscard]] LibraryReader final
  {
  public:
    LibraryReader(LibraryReader&&) noexcept;
    LibraryReader& operator=(LibraryReader&&) noexcept;
    LibraryReader(LibraryReader const&) = delete;
    LibraryReader& operator=(LibraryReader const&) = delete;
    ~LibraryReader();

    bool isValid() const noexcept;

    // Tracks
    std::optional<TrackRow> trackRow(TrackId id) const;
    bool containsTrack(TrackId id) const;
    ResourceId trackCoverArtId(TrackId id) const;
    std::optional<std::filesystem::path> trackUriPath(TrackId id) const;
    TrackFieldRawValue trackField(TrackId id, TrackField field) const;

    // Dictionary
    std::string resolve(DictionaryId id) const;
    std::vector<std::string> resolveAll(std::span<DictionaryId const> ids) const;

    // List tree
    std::vector<ListNode> lists() const;
    std::optional<ListNode> listNode(ListId id) const;
    std::vector<TrackId> listTrackIds(ListId id) const;

    // Resources
    std::optional<std::vector<std::byte>> loadResource(ResourceId id) const;

    // Tags

    // Tags shared by every track in the selection (their intersection). Tracks
    // that do not exist contribute no tags, so a stale id in the selection
    // narrows the result to empty. Order is unspecified.
    std::vector<std::string> selectionTags(std::span<TrackId const> trackIds) const;

    // Every distinct tag in the library paired with how many tracks carry it,
    // ordered by descending frequency then ascending name.
    std::vector<std::pair<std::string, std::size_t>> allTagsByFrequency() const;

  private:
    friend class Library;

    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    explicit LibraryReader(library::MusicLibrary& library);
  };
} // namespace ao::rt
