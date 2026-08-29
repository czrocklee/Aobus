// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/media/ogg/Demuxer.h>

#include "PageLayout.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>
#include <optional>
#include <span>

namespace ao::media::ogg
{
  namespace
  {
    struct PageView final
    {
      PageHeaderLayout const* header = nullptr;
      std::span<std::byte const> segmentTable;
      std::span<std::byte const> payload;
      std::size_t payloadOffset = 0;
      std::size_t totalSize = 0;
    };

    bool beginsPage(std::span<std::byte const> bytes) noexcept
    {
      return bytes.size() >= PageHeaderLayout::kSize &&
             std::ranges::equal(bytes.first(kCapturePattern.size()), utility::bytes::view(kCapturePattern));
    }

    // Resolves one page at an offset already known to begin a capture pattern.
    // Returns nullopt only for a page whose declared extent leaves the file,
    // which is indistinguishable from the file simply ending early.
    std::optional<PageView> viewPage(std::span<std::byte const> fileBytes, std::size_t offset) noexcept
    {
      auto const pageBytes = fileBytes.subspan(offset);
      auto const* const header = utility::layout::view<PageHeaderLayout>(pageBytes);
      auto const segmentCount = static_cast<std::size_t>(header->segmentCount);
      auto const tableOffset = PageHeaderLayout::kSize;

      if (segmentCount > pageBytes.size() - tableOffset)
      {
        return std::nullopt;
      }

      auto const segmentTable = pageBytes.subspan(tableOffset, segmentCount);
      auto const payloadSize =
        std::accumulate(segmentTable.begin(),
                        segmentTable.end(),
                        std::size_t{0},
                        [](std::size_t total, std::byte lacing) { return total + static_cast<std::size_t>(lacing); });
      auto const payloadOffset = tableOffset + segmentCount;

      if (payloadSize > pageBytes.size() - payloadOffset)
      {
        return std::nullopt;
      }

      return PageView{.header = header,
                      .segmentTable = segmentTable,
                      .payload = pageBytes.subspan(payloadOffset, payloadSize),
                      .payloadOffset = offset + payloadOffset,
                      .totalSize = payloadOffset + payloadSize};
    }

    bool continuesPacket(std::span<std::byte const> segmentTable) noexcept
    {
      return !segmentTable.empty() && static_cast<std::uint8_t>(segmentTable.back()) == kContinuationLacingValue;
    }
  } // namespace

  Demuxer::Demuxer(std::span<std::byte const> fileBytes)
    : _fileBytes{fileBytes}
  {
  }

  void Demuxer::appendPagePackets(std::span<std::byte const> segmentTable,
                                  std::span<std::byte const> payload,
                                  std::size_t payloadOffset,
                                  std::size_t pageOffset,
                                  std::optional<PendingPacket>& optPending)
  {
    std::size_t runOffset = 0;
    std::size_t runSize = 0;

    for (auto const lacing : segmentTable)
    {
      runSize += static_cast<std::size_t>(lacing);

      if (static_cast<std::uint8_t>(lacing) == kContinuationLacingValue)
      {
        continue;
      }

      if (optPending)
      {
        auto const run = payload.subspan(runOffset, runSize);
        _reassembled.insert(_reassembled.end(), run.begin(), run.end());
        _packets.push_back(PacketEntry{.offset = optPending->startOffset,
                                       .size = _reassembled.size() - optPending->startOffset,
                                       .pageOffset = optPending->pageOffset,
                                       .isReassembled = true});
        optPending.reset();
      }
      else
      {
        _packets.push_back(PacketEntry{
          .offset = payloadOffset + runOffset, .size = runSize, .pageOffset = pageOffset, .isReassembled = false});
      }

      runOffset += runSize;
      runSize = 0;
    }

    if (!continuesPacket(segmentTable))
    {
      return;
    }

    if (!optPending)
    {
      optPending = PendingPacket{.startOffset = _reassembled.size(), .pageOffset = pageOffset};
    }

    auto const run = payload.subspan(runOffset);
    _reassembled.insert(_reassembled.end(), run.begin(), run.end());
  }

  Result<Demuxer> Demuxer::parse(std::span<std::byte const> fileBytes)
  {
    if (!beginsPage(fileBytes))
    {
      return makeError(Error::Code::CorruptData, "unrecognized ogg file content");
    }

    auto demuxer = Demuxer{fileBytes};
    auto const* const firstHeader = utility::layout::view<PageHeaderLayout>(fileBytes);

    if (firstHeader->version != kSupportedVersion)
    {
      return makeError(Error::Code::CorruptData, "unsupported ogg bitstream version");
    }

    if ((firstHeader->headerType & kBeginOfStreamFlag) == 0)
    {
      return makeError(Error::Code::CorruptData, "ogg file does not begin a logical bitstream");
    }

    demuxer._serialNumber = firstHeader->serialNumber.value();

    auto optPending = std::optional<PendingPacket>{};
    auto optPreviousSequence = std::optional<std::uint32_t>{};
    std::size_t offset = 0;

    while (beginsPage(fileBytes.subspan(offset)))
    {
      auto const optPage = viewPage(fileBytes, offset);

      if (!optPage)
      {
        // A page cut short by the end of the file ends the bitstream. The
        // packets already recovered stay usable and the incomplete tail carries
        // the integrity signal.
        break;
      }

      auto const& page = *optPage;

      if (page.header->serialNumber.value() != demuxer._serialNumber)
      {
        offset += page.totalSize;
        continue;
      }

      if (page.header->version != kSupportedVersion)
      {
        return makeError(Error::Code::CorruptData, "unsupported ogg bitstream version");
      }

      if (optPreviousSequence && (page.header->headerType & kBeginOfStreamFlag) != 0)
      {
        return makeError(Error::Code::CorruptData, "unexpected begin-of-stream flag after the first ogg page");
      }

      if (((page.header->headerType & kContinuedPacketFlag) != 0) != optPending.has_value())
      {
        return makeError(Error::Code::CorruptData, "ogg page continuation flag contradicts the packet in progress");
      }

      // Sequence numbers count pages of this logical bitstream only, so an
      // interleaved foreign stream does not disturb them. They wrap naturally.
      auto const pageSequence = page.header->pageSequence.value();

      if (optPreviousSequence && pageSequence != *optPreviousSequence + 1)
      {
        return makeError(Error::Code::CorruptData, "ogg page sequence skips a page of the logical bitstream");
      }

      optPreviousSequence = pageSequence;

      auto const firstPacketIndex = demuxer._packets.size();
      demuxer.appendPagePackets(page.segmentTable, page.payload, page.payloadOffset, offset, optPending);

      auto const isEndOfStream = (page.header->headerType & kEndOfStreamFlag) != 0;

      if (auto const granulePosition = page.header->granulePosition.value();
          demuxer._packets.size() > firstPacketIndex && granulePosition != kUnsetGranulePosition)
      {
        demuxer._packets.back().granulePosition = granulePosition;
        demuxer._pageEnds.push_back(PageEnd{.firstPacketIndex = firstPacketIndex,
                                            .granulePosition = granulePosition,
                                            .endsOnEndOfStreamPage = isEndOfStream});
        demuxer._finalGranulePosition = granulePosition;
      }

      offset += page.totalSize;

      if (isEndOfStream)
      {
        demuxer._hasIncompleteTail = optPending.has_value();
        return demuxer;
      }
    }

    return demuxer;
  }

  std::uint32_t Demuxer::serialNumber() const noexcept
  {
    return _serialNumber;
  }

  std::size_t Demuxer::packetCount() const noexcept
  {
    return _packets.size();
  }

  Demuxer::Packet Demuxer::packet(std::size_t index) const noexcept
  {
    AO_EXPECTS(index < _packets.size());
    auto const& entry = _packets[index];
    auto const source = entry.isReassembled ? std::span<std::byte const>{_reassembled} : _fileBytes;
    return Packet{.bytes = source.subspan(entry.offset, entry.size), .granulePosition = entry.granulePosition};
  }

  std::size_t Demuxer::packetPageOffset(std::size_t index) const noexcept
  {
    AO_EXPECTS(index < _packets.size());
    return _packets[index].pageOffset;
  }

  std::int64_t Demuxer::finalGranulePosition() const noexcept
  {
    return _finalGranulePosition;
  }

  std::size_t Demuxer::pageGroupCount() const noexcept
  {
    return _pageEnds.size();
  }

  Demuxer::PageGroup Demuxer::pageGroup(std::size_t index) const noexcept
  {
    AO_EXPECTS(index < _pageEnds.size());
    auto const& entry = _pageEnds[index];
    auto const endPacketIndex = index + 1 < _pageEnds.size() ? _pageEnds[index + 1].firstPacketIndex : _packets.size();
    return PageGroup{.firstPacketIndex = entry.firstPacketIndex,
                     .packetCount = endPacketIndex - entry.firstPacketIndex,
                     .granulePosition = entry.granulePosition,
                     .endsOnEndOfStreamPage = entry.endsOnEndOfStreamPage};
  }

  Demuxer::Restart Demuxer::restartAtGranule(std::int64_t granulePosition) const noexcept
  {
    if (_pageEnds.empty())
    {
      return Restart{.packetIndex = 0, .granulePosition = 0};
    }

    auto const iterator = std::ranges::lower_bound(_pageEnds, granulePosition, {}, &PageEnd::granulePosition);

    if (iterator == _pageEnds.end())
    {
      return Restart{.packetIndex = _packets.size(), .granulePosition = _finalGranulePosition};
    }

    // The preceding page group ends where this one begins. Reading it from the
    // page index rather than from a packet keeps the answer exact even when a
    // page completes packets without declaring a granule position.
    auto const resumeGranule = iterator == _pageEnds.begin() ? std::int64_t{0} : std::prev(iterator)->granulePosition;
    return Restart{.packetIndex = iterator->firstPacketIndex, .granulePosition = resumeGranule};
  }

  bool Demuxer::hasIncompleteTail() const noexcept
  {
    return _hasIncompleteTail;
  }
} // namespace ao::media::ogg
