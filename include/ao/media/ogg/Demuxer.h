// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ao::media::ogg
{
  /**
   * @brief Demuxer for the packets of one Ogg logical bitstream.
   *
   * The first page of the file must begin a logical bitstream; its serial
   * number selects the demuxed stream and pages carrying any other serial are
   * skipped, so a multiplexed file exposes only its first logical bitstream.
   * Demuxing stops at that stream's end-of-stream page, so a chained file
   * exposes only its first link.
   *
   * Page checksums are not verified; the capture pattern, the bitstream
   * version, begin-of-stream placement, and the page sequence are. A sequence
   * gap in the selected stream is rejected rather than tolerated: the resume
   * position of a seek is read from where the preceding page ended, so a missing page would silently
   * displace every later seek, and a packet spanning the gap would be
   * reassembled from unrelated fragments. Demuxing stops cleanly at the first position that does not
   * begin a page and at a page cut short by the end of the file, so neither
   * trailing bytes such as an appended tag nor a truncated download rejects the
   * whole file. An unterminated trailing packet is dropped, and every
   * incomplete ending is reported through hasIncompleteTail().
   *
   * Packet bytes borrow the parsed file buffer while every packet that spans
   * more than one page is reassembled into demuxer-owned storage. Both remain
   * valid while the demuxer lives and survive moving it.
   */
  class Demuxer final
  {
  public:
    // Granule position of a packet that is not the last one completed on its
    // page, matching the "unset" convention Ogg uses on the wire.
    static constexpr std::int64_t kUnsetGranulePosition = -1;

    struct Packet final
    {
      std::span<std::byte const> bytes;
      std::int64_t granulePosition = kUnsetGranulePosition;
    };

    ~Demuxer() = default;

    Demuxer(Demuxer const&) = delete;
    Demuxer& operator=(Demuxer const&) = delete;
    Demuxer(Demuxer&&) noexcept = default;
    Demuxer& operator=(Demuxer&&) = delete;

    /**
     * @brief Parses the first logical bitstream of an Ogg file.
     * @return A ready-to-use demuxer, or Error::Code::CorruptData when the
     *         page structure, capture pattern, or version is unusable.
     */
    static Result<Demuxer> parse(std::span<std::byte const> fileBytes);

    /**
     * @brief Serial number of the demuxed logical bitstream.
     */
    std::uint32_t serialNumber() const noexcept;

    /**
     * @brief Number of complete packets recovered from the logical bitstream.
     */
    std::size_t packetCount() const noexcept;

    /**
     * @brief Returns the bytes and end-of-page granule position of one packet.
     *
     * The index must be below packetCount().
     */
    Packet packet(std::size_t index) const noexcept;

    /**
     * @brief File offset of the page the given packet starts on.
     *
     * A caller that must name a stable byte range for a run of packets uses
     * this rather than the packet bytes, whose storage moves into demuxer-owned
     * memory once a packet spans pages. The index must be below packetCount().
     */
    std::size_t packetPageOffset(std::size_t index) const noexcept;

    /**
     * @brief Granule position of the last page of the logical bitstream, or
     *        kUnsetGranulePosition when no page carried one.
     */
    std::int64_t finalGranulePosition() const noexcept;

    /**
     * @brief The packets that complete on one page, and where that page ends.
     *
     * Pages are grouped rather than exposed directly because a granule position
     * describes the page as a whole. A page that completes no packet, or that
     * declares no granule position, forms no group.
     */
    struct PageGroup final
    {
      std::size_t firstPacketIndex = 0;
      std::size_t packetCount = 0;
      std::int64_t granulePosition = kUnsetGranulePosition;
      // Whether the page closing this group carried the end-of-stream flag.
      bool endsOnEndOfStreamPage = false;
    };

    /**
     * @brief Number of pages that completed at least one packet and declared a
     *        granule position.
     */
    std::size_t pageGroupCount() const noexcept;

    /**
     * @brief Returns one page group. The index must be below pageGroupCount().
     */
    PageGroup pageGroup(std::size_t index) const noexcept;

    struct Restart final
    {
      std::size_t packetIndex = 0;
      // Decode position the packet resumes from, which a caller subtracts from
      // its target to know how much decoded audio to discard.
      std::int64_t granulePosition = 0;
    };

    /**
     * @brief Maps a granule position to the point decoding must restart from.
     *
     * The restart is the first packet of the page group whose end granule
     * position first reaches the requested position, which is the earliest point
     * that can produce it, paired with the granule position that group begins
     * at. A position beyond the end of the stream restarts at packetCount().
     */
    Restart restartAtGranule(std::int64_t granulePosition) const noexcept;

    /**
     * @brief Whether the logical bitstream ended incompletely.
     *
     * True when no end-of-stream page closed the bitstream or when its last
     * page left a packet unterminated, which is the signal that the file was
     * truncated rather than merely followed by unrelated bytes.
     */
    bool hasIncompleteTail() const noexcept;

  private:
    struct PacketEntry final
    {
      std::size_t offset = 0;
      std::size_t size = 0;
      std::size_t pageOffset = 0;
      std::int64_t granulePosition = kUnsetGranulePosition;
      bool isReassembled = false;
    };

    struct PageEnd final
    {
      std::size_t firstPacketIndex = 0;
      std::int64_t granulePosition = kUnsetGranulePosition;
      bool endsOnEndOfStreamPage = false;
    };

    // A packet still being assembled from the pages it spans.
    struct PendingPacket final
    {
      // Where the packet begins in _reassembled.
      std::size_t startOffset = 0;
      // File offset of the page the packet began on.
      std::size_t pageOffset = 0;
    };

    explicit Demuxer(std::span<std::byte const> fileBytes);

    // Appends every packet the page terminates, closing one carried in from an
    // earlier page and carrying any unterminated tail back out.
    void appendPagePackets(std::span<std::byte const> segmentTable,
                           std::span<std::byte const> payload,
                           std::size_t payloadOffset,
                           std::size_t pageOffset,
                           std::optional<PendingPacket>& optPending);

    std::span<std::byte const> _fileBytes;
    std::vector<std::byte> _reassembled;
    std::vector<PacketEntry> _packets;
    std::vector<PageEnd> _pageEnds;
    std::uint32_t _serialNumber = 0;
    std::int64_t _finalGranulePosition = kUnsetGranulePosition;
    bool _hasIncompleteTail = true;
  };
} // namespace ao::media::ogg
