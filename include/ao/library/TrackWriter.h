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

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ao::library
{
  class FileManifestStore;
  class WriteTransaction;

  /** An absent number is unchanged; a present zero clears it. */
  struct TrackNumberPatch final
  {
    std::optional<std::uint16_t> optYear{};
    std::optional<std::uint16_t> optTrackNumber{};
    std::optional<std::uint16_t> optTrackTotal{};
    std::optional<std::uint16_t> optDiscNumber{};
    std::optional<std::uint16_t> optDiscTotal{};
  };

  /**
   * Transaction-scoped logical mutation port for Track and manifest state.
   * Every operation belongs inside the owning WriteTransaction::apply() root.
   * The writer borrows that transaction and must not outlive it.
   */
  class TrackWriter final
  {
  public:
    enum class EditTier : std::uint8_t
    {
      Hot,
      Complete,
    };

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

    /**
     * Load an editable baseline from this writer, including dictionary text staged
     * earlier in the transaction. Returns nullopt only for a missing Track.
     * The builder borrows its inputs and retains library-local Resource IDs:
     * consume it in this write before another mutation or the end of this
     * apply operation. A Hot baseline is only for updateHot().
     */
    std::optional<TrackBuilder> loadForEdit(TrackId id, EditTier tier = EditTier::Complete) const;

    /** Looks up one manifest by canonical, root-relative LibraryUri text. */
    std::optional<FileManifestView> manifest(std::string_view uri) const;

    /**
     * The returned id is final only once the transaction commits; an aborted
     * or failed transaction's ids may be allocated again.
     */
    Result<TrackId> create(TrackBuilder const& track, FileManifestBuilder manifest);
    /** Returns provisional ids in builder input order; manifest membership is in ordinal order. */
    Result<std::vector<TrackId>> createCarrier(std::span<TrackBuilder const> tracks,
                                               FileManifestBuilder manifest,
                                               std::uint64_t totalFrames);
    /** Returns new provisional ids in builder input order, not manifest ordinal order. */
    Result<std::vector<TrackId>> addCarrierMembers(std::string_view uri, std::span<TrackBuilder const> tracks);
    Result<> relinkCarrier(std::string_view oldUri, std::string_view newUri, FileManifestBuilder manifest);
    Result<> updateManifest(std::string_view uri, FileManifestBuilder manifest);
    Result<> update(TrackId id, TrackBuilder const& track);
    /**
     * Updates only hot fields. A complete carrier builder must still preserve
     * its URI, segment and duration; every carrier builder must preserve sample rate.
     */
    Result<> updateHot(TrackId id, TrackBuilder const& track);
    Result<> updateCold(TrackId id, TrackBuilder const& track);
    /**
     * Returns the previous values of just the fields that changed, or NotFound
     * for a missing Track. An empty or already satisfied patch writes nothing.
     */
    Result<TrackNumberPatch> updateNumbers(TrackId id, TrackNumberPatch const& patch);
    Result<> replace(TrackId id, TrackBuilder const& track, FileManifestBuilder manifest);
    Result<> updateManifest(TrackId id, FileManifestBuilder manifest);
    Result<> relink(TrackId id, TrackBuilder const& track, FileManifestBuilder manifest);

    Result<bool> remove(TrackId id);
    Result<> clear();

  private:
    explicit TrackWriter(WriteTransaction& transaction);
    void requireActiveOperation() const;
    Result<> validateResourceReferences(TrackBuilder const& track) const;
    Result<> validateStructure(TrackId id, TrackBuilder const& track, bool hotOnly = false) const;

    WriteTransaction* _transaction;

    friend class WriteTransaction;
  };
} // namespace ao::library
