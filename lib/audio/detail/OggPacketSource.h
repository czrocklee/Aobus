// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/media/ogg/Demuxer.h>
#include <ao/utility/MappedFile.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace ao::audio::detail
{
  /**
   * @brief Sequential cursor over the packets of one Ogg logical bitstream.
   *
   * The source owns the mapping and the demuxer so that packet bytes stay valid
   * for the whole session, mirroring how Mp4PacketSource serves MP4 samples.
   * It carries no codec knowledge; a caller identifies its own header packets by
   * index and drives the cursor from there.
   */
  class OggPacketSource final
  {
  public:
    Result<> open(std::filesystem::path const& filePath);
    void close() noexcept;

    /**
     * @brief Positions the cursor at the earliest packet that can produce the
     *        requested granule position.
     * @return The granule position that packet resumes decoding from, which a
     *         caller compares against its target to know how much decoded audio
     *         to discard.
     */
    std::int64_t seekToGranule(std::int64_t granulePosition) noexcept;

    bool isOpen() const noexcept;
    bool isAtEnd() const noexcept;

    std::span<std::byte const> packet() const;
    std::span<std::byte const> packetAt(std::size_t index) const;

    std::size_t packetIndex() const noexcept;
    std::size_t packetCount() const noexcept;
    std::int64_t finalGranulePosition() const noexcept;

    // The parsed container. Valid only while the source is open, which the
    // Opus layer establishes before deriving its timeline from it.
    media::ogg::Demuxer const& demuxer() const noexcept;

    void setPacketIndex(std::size_t index) noexcept;
    void advance() noexcept;

  private:
    utility::MappedFile _mappedFile;
    std::optional<media::ogg::Demuxer> _optDemuxer;
    std::size_t _packetIndex = 0;
  };
} // namespace ao::audio::detail
