// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestView.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>

#include <optional>
#include <string_view>

namespace ao::library
{
  class FileManifestStore;
  class WriteTransaction;

  /**
   * Transaction-scoped logical mutation port for Track and manifest state.
   * Every operation belongs inside the owning WriteTransaction::apply() root.
   * The writer borrows that transaction and must not outlive it.
   */
  class TrackWriter final
  {
  public:
    ~TrackWriter() = default;

    TrackWriter(TrackWriter const&) = delete;
    TrackWriter& operator=(TrackWriter const&) = delete;
    TrackWriter(TrackWriter&&) noexcept = default;
    TrackWriter& operator=(TrackWriter&&) noexcept = default;

    /**
     * Read-only preflight for a complete Track candidate and existing Resource
     * references. A hot-only builder returns InvalidInput.
     */
    Result<> validate(TrackBuilder const& track) const;

    std::optional<TrackView> get(TrackId id,
                                 TrackStore::Reader::LoadMode mode = TrackStore::Reader::LoadMode::Both) const;
    /** Looks up one manifest by canonical, root-relative LibraryUri text. */
    std::optional<FileManifestView> manifest(std::string_view uri) const;

    Result<TrackId> create(TrackBuilder const& track, FileManifestBuilder manifest);
    Result<> update(TrackId id, TrackBuilder const& track);
    /** Updates only hot fields; cold fields, including URI, are not consumed. */
    Result<> updateHot(TrackId id, TrackBuilder const& track);
    Result<> updateCold(TrackId id, TrackBuilder const& track);
    Result<> replace(TrackId id, TrackBuilder const& track, FileManifestBuilder manifest);
    Result<> updateManifest(TrackId id, FileManifestBuilder manifest);
    Result<> relink(TrackId id, TrackBuilder const& track, FileManifestBuilder manifest);

    Result<bool> remove(TrackId id);
    Result<> clear();

  private:
    explicit TrackWriter(WriteTransaction& transaction);
    void requireActiveOperation() const;
    Result<> validateResourceReferences(TrackBuilder const& track) const;

    WriteTransaction* _transaction;

    friend class WriteTransaction;
  };
} // namespace ao::library
