// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/media/detail/MediaError.h>
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/AtomLayout.h>
#include <ao/media/mp4/Demuxer.h>
#include <ao/media/mp4/TrackSelection.h>
#include <ao/utility/ByteView.h>

#include <boost/endian/buffers.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::media::mp4
{
  namespace
  {
    constexpr std::byte kEsDescriptorTag{0x03};
    constexpr std::byte kDecoderConfigDescriptorTag{0x04};
    constexpr std::byte kDecoderSpecificInfoTag{0x05};
    constexpr std::uint8_t kDescriptorLengthBitsPerByte = 7;
    constexpr std::uint8_t kDescriptorLengthByteMask = 0x7F;
    constexpr std::uint8_t kDescriptorLengthContinuationBit = 0x80;
    constexpr std::uint8_t kEsStreamDependenceFlag = 0x80;
    constexpr std::uint8_t kEsUrlFlag = 0x40;

    struct MdhdVersion0BodyLayout final
    {
      boost::endian::big_uint32_buf_t versionAndFlags;
      boost::endian::big_uint32_buf_t creationTime;
      boost::endian::big_uint32_buf_t modificationTime;
      boost::endian::big_uint32_buf_t timescale;
      boost::endian::big_uint32_buf_t duration;
    };

    constexpr std::size_t kMdhdVersion0BodySize = 20;
    static_assert(sizeof(MdhdVersion0BodyLayout) == kMdhdVersion0BodySize);
    static_assert(alignof(MdhdVersion0BodyLayout) == 1);
    static_assert(utility::layout::kIsBinaryLayoutType<MdhdVersion0BodyLayout>);

    struct MdhdVersion1BodyLayout final
    {
      boost::endian::big_uint32_buf_t versionAndFlags;
      boost::endian::big_uint64_buf_t creationTime;
      boost::endian::big_uint64_buf_t modificationTime;
      boost::endian::big_uint32_buf_t timescale;
      boost::endian::big_uint64_buf_t duration;
    };

    static_assert(sizeof(MdhdVersion1BodyLayout) == 32);
    static_assert(alignof(MdhdVersion1BodyLayout) == 1);
    static_assert(utility::layout::kIsBinaryLayoutType<MdhdVersion1BodyLayout>);

    struct EntryCountBodyLayout final
    {
      boost::endian::big_uint32_buf_t versionAndFlags;
      boost::endian::big_uint32_buf_t entryCount;
    };

    static_assert(sizeof(EntryCountBodyLayout) == 8);
    static_assert(alignof(EntryCountBodyLayout) == 1);
    static_assert(utility::layout::kIsBinaryLayoutType<EntryCountBodyLayout>);

    struct SampleSizeBodyLayout final
    {
      boost::endian::big_uint32_buf_t versionAndFlags;
      boost::endian::big_uint32_buf_t sampleSize;
      boost::endian::big_uint32_buf_t sampleCount;
    };

    constexpr std::size_t kSampleSizeBodySize = 12;
    static_assert(sizeof(SampleSizeBodyLayout) == kSampleSizeBodySize);
    static_assert(alignof(SampleSizeBodyLayout) == 1);
    static_assert(utility::layout::kIsBinaryLayoutType<SampleSizeBodyLayout>);

    struct TrackTiming final
    {
      std::uint32_t timescale = 0;
      std::uint64_t duration = 0;
    };

    template<typename Header, typename Entry>
    std::optional<std::span<Entry const>> validatedEntries(std::span<std::byte const> bytes, std::uint32_t count)
    {
      if (bytes.size() < sizeof(Header))
      {
        return std::nullopt;
      }

      auto const entryBytes = bytes.subspan(sizeof(Header));

      if ((entryBytes.size() % sizeof(Entry)) != 0 || entryBytes.size() / sizeof(Entry) != count)
      {
        return std::nullopt;
      }

      return utility::layout::viewArray<Entry>(entryBytes);
    }

    std::uint8_t byteValue(std::byte byte) noexcept
    {
      return static_cast<std::uint8_t>(byte);
    }

    std::optional<std::size_t> readDescriptorLength(std::span<std::byte const> bytes, std::size_t& offset) noexcept
    {
      std::size_t length = 0;

      for (std::size_t i = 0; i < 4; ++i)
      {
        if (offset >= bytes.size())
        {
          return std::nullopt;
        }

        auto const value = byteValue(bytes[offset++]);
        length = (length << kDescriptorLengthBitsPerByte) | (value & kDescriptorLengthByteMask);

        if ((value & kDescriptorLengthContinuationBit) == 0)
        {
          return length;
        }
      }

      return std::nullopt;
    }

    std::span<std::byte const> skipEsDescriptorHeader(std::span<std::byte const> payload) noexcept
    {
      constexpr std::size_t kBaseHeaderSize = 3;

      if (payload.size() < kBaseHeaderSize)
      {
        return {};
      }

      auto offset = kBaseHeaderSize;
      auto const flags = byteValue(payload[2]);

      if ((flags & kEsStreamDependenceFlag) != 0)
      {
        offset += 2;
      }

      if ((flags & kEsUrlFlag) != 0)
      {
        if (offset >= payload.size())
        {
          return {};
        }

        offset += 1U + byteValue(payload[offset]);
      }

      if ((flags & 0x20U) != 0)
      {
        offset += 2;
      }

      if (offset > payload.size())
      {
        return {};
      }

      return payload.subspan(offset);
    }

    std::span<std::byte const> skipDecoderConfigDescriptorHeader(std::span<std::byte const> payload) noexcept
    {
      constexpr std::size_t kHeaderSize = 13;

      if (payload.size() < kHeaderSize)
      {
        return {};
      }

      return payload.subspan(kHeaderSize);
    }

    std::optional<std::vector<std::byte>> findDescriptorPayload(std::span<std::byte const> bytes, std::byte targetTag)
    {
      std::size_t offset = 0;

      while (offset < bytes.size())
      {
        auto const tag = bytes[offset++];
        auto const optLength = readDescriptorLength(bytes, offset);

        if (!optLength || offset + *optLength > bytes.size())
        {
          return std::nullopt;
        }

        auto const payload = bytes.subspan(offset, *optLength);

        if (tag == targetTag)
        {
          return std::vector<std::byte>{payload.begin(), payload.end()};
        }

        auto nested = std::span<std::byte const>{};

        if (tag == kEsDescriptorTag)
        {
          nested = skipEsDescriptorHeader(payload);
        }
        else if (tag == kDecoderConfigDescriptorTag)
        {
          nested = skipDecoderConfigDescriptorHeader(payload);
        }

        if (!nested.empty())
        {
          if (auto optPayload = findDescriptorPayload(nested, targetTag); optPayload)
          {
            return optPayload;
          }
        }

        offset += *optLength;
      }

      return std::nullopt;
    }

    std::vector<std::byte> extractAacMagicCookie(AtomView const& esdsView)
    {
      auto const payload = esdsView.payload();

      constexpr std::size_t kFullBoxHeaderSize = 4;

      if (payload.size() <= kFullBoxHeaderSize)
      {
        return {};
      }

      auto const descriptors = payload.subspan(kFullBoxHeaderSize);
      auto optCookie = findDescriptorPayload(descriptors, kDecoderSpecificInfoTag);
      return optCookie.value_or(std::vector<std::byte>{});
    }

    OptionalAtom findAtomOrThrow(AtomView const& root, std::span<std::string_view const> path)
    {
      auto nodeResult = findAtom(root, path);

      if (!nodeResult)
      {
        detail::throwMediaError(Error::Code::FormatRejected, nodeResult.error().message);
      }

      return *nodeResult;
    }

    TrackTiming parseTrackTiming(AtomView const& track)
    {
      static constexpr auto kMdhdPath = std::to_array<std::string_view>({
        "trak",
        "mdia",
        "mdhd",
      });

      auto timing = TrackTiming{};
      auto const optNode = findAtomOrThrow(track, kMdhdPath);

      if (!optNode)
      {
        return timing;
      }

      auto const payload = optNode->payload();

      if (payload.size() < sizeof(std::uint32_t))
      {
        detail::throwMediaError(Error::Code::FormatRejected, "Malformed mdhd atom");
      }

      auto const version = std::to_integer<std::uint8_t>(payload[0]);

      if (version == 0)
      {
        if (payload.size() < sizeof(MdhdVersion0BodyLayout))
        {
          detail::throwMediaError(Error::Code::FormatRejected, "Malformed version 0 mdhd atom");
        }

        auto const* layout = utility::layout::view<MdhdVersion0BodyLayout>(payload);
        timing.timescale = layout->timescale.value();
        timing.duration = layout->duration.value();
        return timing;
      }

      if (version == 1)
      {
        if (payload.size() < sizeof(MdhdVersion1BodyLayout))
        {
          detail::throwMediaError(Error::Code::FormatRejected, "Malformed version 1 mdhd atom");
        }

        auto const* layout = utility::layout::view<MdhdVersion1BodyLayout>(payload);
        timing.timescale = layout->timescale.value();
        timing.duration = layout->duration.value();
        return timing;
      }

      detail::throwMediaError(Error::Code::FormatRejected, "Unsupported mdhd version");
    }
  } // namespace

  Demuxer::Demuxer(std::span<std::byte const> fileData)
    : _fileData{fileData}
  {
  }

  void Demuxer::applySampleTiming(std::vector<SampleEntry>& samples, std::span<TimeToSampleEntry const> timeToSample)
  {
    if (samples.empty() || timeToSample.empty())
    {
      detail::throwMediaError(Error::Code::FormatRejected, "Missing MP4 samples or timing entries");
    }

    std::size_t sampleIndex = 0;
    std::uint64_t sampleTime = 0;

    for (auto const& entry : timeToSample)
    {
      if (entry.sampleCount == 0 || entry.sampleDelta == 0)
      {
        detail::throwMediaError(Error::Code::FormatRejected, "Invalid MP4 time-to-sample entry");
      }

      for (std::uint32_t sample = 0; sample < entry.sampleCount; ++sample)
      {
        if (sampleIndex >= samples.size())
        {
          detail::throwMediaError(Error::Code::FormatRejected, "MP4 timing table has too many samples");
        }

        samples[sampleIndex].startTime = sampleTime;
        samples[sampleIndex].duration = entry.sampleDelta;
        sampleTime += entry.sampleDelta;
        ++sampleIndex;
      }
    }

    if (sampleIndex != samples.size())
    {
      detail::throwMediaError(Error::Code::FormatRejected, "MP4 timing table does not cover every sample");
    }
  }

  void Demuxer::buildSampleOffsets(std::vector<SampleEntry>& samples,
                                   std::span<std::uint64_t const> chunkOffsets,
                                   std::span<SampleToChunkEntry const> sampleToChunk)
  {
    if (samples.empty() || chunkOffsets.empty() || sampleToChunk.empty())
    {
      detail::throwMediaError(
        Error::Code::FormatRejected, "Missing MP4 samples, chunk offsets, or sample-to-chunk table");
    }

    std::size_t sampleIndex = 0;

    for (std::size_t entryIndex = 0; entryIndex < sampleToChunk.size(); ++entryIndex)
    {
      auto const& entry = sampleToChunk[entryIndex];

      if (entry.firstChunk == 0 || entry.samplesPerChunk == 0 || entry.sampleDescriptionIndex != 1)
      {
        detail::throwMediaError(Error::Code::FormatRejected, "Invalid MP4 sample-to-chunk entry");
      }

      auto const chunkStartIndex = static_cast<std::size_t>(entry.firstChunk - 1);

      if ((entryIndex == 0 && entry.firstChunk != 1) || chunkStartIndex >= chunkOffsets.size())
      {
        detail::throwMediaError(Error::Code::FormatRejected, "MP4 sample-to-chunk entry references an invalid chunk");
      }

      std::size_t chunkEndIndex = chunkOffsets.size();

      if (entryIndex + 1 < sampleToChunk.size())
      {
        auto const nextFirstChunk = sampleToChunk[entryIndex + 1].firstChunk;

        if (nextFirstChunk <= entry.firstChunk)
        {
          detail::throwMediaError(Error::Code::FormatRejected, "MP4 sample-to-chunk entries are not ordered");
        }

        chunkEndIndex = std::min(chunkEndIndex, static_cast<std::size_t>(nextFirstChunk - 1));
      }

      for (auto chunkIndex = chunkStartIndex; chunkIndex < chunkEndIndex; ++chunkIndex)
      {
        auto sampleOffset = chunkOffsets[chunkIndex];

        for (std::uint32_t sampleInChunk = 0; sampleInChunk < entry.samplesPerChunk; ++sampleInChunk)
        {
          if (sampleIndex >= samples.size())
          {
            detail::throwMediaError(Error::Code::FormatRejected, "MP4 sample-to-chunk table has too many samples");
          }

          samples[sampleIndex].offset = sampleOffset;

          if (samples[sampleIndex].size > std::numeric_limits<std::uint64_t>::max() - sampleOffset)
          {
            detail::throwMediaError(Error::Code::FormatRejected, "MP4 sample offset overflow");
          }

          sampleOffset += samples[sampleIndex].size;
          ++sampleIndex;
        }
      }
    }

    if (sampleIndex != samples.size())
    {
      detail::throwMediaError(Error::Code::FormatRejected, "MP4 sample-to-chunk table does not cover every sample");
    }
  }

  void Demuxer::parseStts(std::span<std::byte const> bytes, std::vector<TimeToSampleEntry>& out)
  {
    if (bytes.size() < sizeof(EntryCountBodyLayout))
    {
      detail::throwMediaError(Error::Code::FormatRejected, "Malformed stts atom");
    }

    auto const* header = utility::layout::view<EntryCountBodyLayout>(bytes);
    auto const count = header->entryCount.value();
    auto const optEntries = validatedEntries<EntryCountBodyLayout, SttsAtomLayout::Entry>(bytes, count);

    if (!optEntries)
    {
      detail::throwMediaError(Error::Code::FormatRejected, "Malformed stts entry table");
    }

    out.resize(count);

    for (auto const& [index, entry] : std::ranges::views::enumerate(*optEntries))
    {
      auto const unsignedIndex = static_cast<std::size_t>(index);
      out[unsignedIndex].sampleCount = entry.sampleCount.value();
      out[unsignedIndex].sampleDelta = entry.sampleDelta.value();
    }
  }

  void Demuxer::parseStsz(std::span<std::byte const> bytes)
  {
    if (bytes.size() < sizeof(SampleSizeBodyLayout))
    {
      detail::throwMediaError(Error::Code::FormatRejected, "Malformed stsz atom");
    }

    auto const* header = utility::layout::view<SampleSizeBodyLayout>(bytes);
    auto const sampleSize = header->sampleSize.value();

    if (auto const count = header->sampleCount.value(); sampleSize == 0)
    {
      auto const optEntries = validatedEntries<SampleSizeBodyLayout, StszAtomLayout::Entry>(bytes, count);

      if (!optEntries)
      {
        detail::throwMediaError(Error::Code::FormatRejected, "Malformed stsz entry table");
      }

      _samples.resize(count);

      for (auto const& [index, entry] : std::ranges::views::enumerate(*optEntries))
      {
        _samples[static_cast<std::size_t>(index)].size = entry.size.value();
      }
    }
    else
    {
      if (bytes.size() != sizeof(SampleSizeBodyLayout))
      {
        detail::throwMediaError(Error::Code::FormatRejected, "Malformed fixed-size stsz atom");
      }

      _samples.resize(count);

      for (auto& sample : _samples)
      {
        sample.size = sampleSize;
      }
    }
  }

  void Demuxer::parseStsc(std::span<std::byte const> bytes, std::vector<SampleToChunkEntry>& out)
  {
    if (bytes.size() < sizeof(EntryCountBodyLayout))
    {
      detail::throwMediaError(Error::Code::FormatRejected, "Malformed stsc atom");
    }

    auto const* header = utility::layout::view<EntryCountBodyLayout>(bytes);
    auto const count = header->entryCount.value();
    auto const optEntries = validatedEntries<EntryCountBodyLayout, StscAtomLayout::Entry>(bytes, count);

    if (!optEntries)
    {
      detail::throwMediaError(Error::Code::FormatRejected, "Malformed stsc entry table");
    }

    out.resize(count);

    for (auto const& [index, entry] : std::ranges::views::enumerate(*optEntries))
    {
      auto const unsignedIndex = static_cast<std::size_t>(index);
      out[unsignedIndex].firstChunk = entry.firstChunk.value();
      out[unsignedIndex].samplesPerChunk = entry.samplesPerChunk.value();
      out[unsignedIndex].sampleDescriptionIndex = entry.sampleDescIndex.value();
    }
  }

  void Demuxer::parseStco(std::span<std::byte const> bytes, std::vector<std::uint64_t>& out)
  {
    if (bytes.size() < sizeof(EntryCountBodyLayout))
    {
      detail::throwMediaError(Error::Code::FormatRejected, "Malformed stco atom");
    }

    auto const* header = utility::layout::view<EntryCountBodyLayout>(bytes);
    auto const count = header->entryCount.value();
    auto const optEntries = validatedEntries<EntryCountBodyLayout, StcoAtomLayout::Entry>(bytes, count);

    if (!optEntries)
    {
      detail::throwMediaError(Error::Code::FormatRejected, "Malformed stco entry table");
    }

    out.resize(count);

    for (auto const& [index, entry] : std::ranges::views::enumerate(*optEntries))
    {
      out[static_cast<std::size_t>(index)] = entry.chunkOffset.value();
    }
  }

  void Demuxer::parseCo64(std::span<std::byte const> bytes, std::vector<std::uint64_t>& out)
  {
    if (bytes.size() < sizeof(EntryCountBodyLayout))
    {
      detail::throwMediaError(Error::Code::FormatRejected, "Malformed co64 atom");
    }

    auto const* header = utility::layout::view<EntryCountBodyLayout>(bytes);
    auto const count = header->entryCount.value();
    auto const optEntries = validatedEntries<EntryCountBodyLayout, Co64AtomLayout::Entry>(bytes, count);

    if (!optEntries)
    {
      detail::throwMediaError(Error::Code::FormatRejected, "Malformed co64 entry table");
    }

    out.resize(count);

    for (auto const& [index, entry] : std::ranges::views::enumerate(*optEntries))
    {
      out[static_cast<std::size_t>(index)] = entry.chunkOffset.value();
    }
  }

  void Demuxer::parseSampleTable(AtomView const& table,
                                 std::vector<std::uint64_t>& chunkOffsets,
                                 std::vector<SampleToChunkEntry>& sampleToChunk,
                                 std::vector<TimeToSampleEntry>& timeToSample)
  {
    auto const visitResult = visitChildren(table,
                                           [this, &chunkOffsets, &sampleToChunk, &timeToSample](AtomView const& atom)
                                           {
                                             auto type = atom.type();

                                             if (auto const atomPayload = atom.payload(); type == "stsz")
                                             {
                                               parseStsz(atomPayload);
                                             }
                                             else if (type == "stts")
                                             {
                                               parseStts(atomPayload, timeToSample);
                                             }
                                             else if (type == "stsc")
                                             {
                                               parseStsc(atomPayload, sampleToChunk);
                                             }
                                             else if (type == "stco")
                                             {
                                               parseStco(atomPayload, chunkOffsets);
                                             }
                                             else if (type == "co64")
                                             {
                                               parseCo64(atomPayload, chunkOffsets);
                                             }

                                             return true;
                                           });

    if (!visitResult)
    {
      detail::throwMediaError(Error::Code::FormatRejected, visitResult.error().message);
    }
  }

  Result<> Demuxer::parseTrack(std::string_view targetFormat)
  {
    _magicCookie.clear();
    _samples.clear();
    _timescale = 0;
    _duration = 0;

    auto parse = [&] -> Result<>
    {
      auto const root = fromBuffer(_fileData);
      auto chunkOffsets = std::vector<std::uint64_t>{};
      auto sampleToChunk = std::vector<SampleToChunkEntry>{};
      auto timeToSample = std::vector<TimeToSampleEntry>{};

      auto const selectionResult = findAudioTrack(root, targetFormat);

      if (!selectionResult)
      {
        return makeError(Error::Code::FormatRejected, selectionResult.error().message);
      }

      auto const& track = selectionResult->track;

      auto const timing = parseTrackTiming(track);
      _timescale = timing.timescale;
      _duration = timing.duration;

      auto const kCookiePath = std::to_array<std::string_view>({
        "trak",
        "mdia",
        "minf",
        "stbl",
        "stsd",
        targetFormat,
        targetFormat,
      });

      if (auto const optNode = findAtomOrThrow(track, kCookiePath); optNode)
      {
        auto const bytes = optNode->bytes();
        _magicCookie.assign(bytes.begin(), bytes.end());
      }

      if (targetFormat == "mp4a")
      {
        static constexpr auto kEsdsPath = std::to_array<std::string_view>({
          "trak",
          "mdia",
          "minf",
          "stbl",
          "stsd",
          "mp4a",
          "esds",
        });

        if (auto const optNode = findAtomOrThrow(track, kEsdsPath); optNode)
        {
          _magicCookie = extractAacMagicCookie(*optNode);
        }
      }

      static constexpr auto kStblPath = std::to_array<std::string_view>({
        "trak",
        "mdia",
        "minf",
        "stbl",
      });

      auto const optStbl = findAtomOrThrow(track, kStblPath);

      if (!optStbl)
      {
        return makeError(Error::Code::FormatRejected, "Missing stbl atom");
      }

      parseSampleTable(*optStbl, chunkOffsets, sampleToChunk, timeToSample);

      if (_magicCookie.empty() || _samples.empty())
      {
        return makeError(Error::Code::FormatRejected, "Failed to extract track extradata or sample table");
      }

      buildSampleOffsets(_samples, chunkOffsets, sampleToChunk);

      if (!timeToSample.empty())
      {
        applySampleTiming(_samples, timeToSample);
      }

      return {};
    };

    try
    {
      return parse();
    }
    catch (detail::MediaException const& ex)
    {
      return std::unexpected{ex.error()};
    }
  }

  std::span<std::byte const> Demuxer::magicCookie() const
  {
    return _magicCookie;
  }

  std::uint32_t Demuxer::sampleCount() const
  {
    return static_cast<std::uint32_t>(_samples.size());
  }

  Demuxer::SampleEntry Demuxer::sampleInfo(std::uint32_t index) const
  {
    if (index >= _samples.size())
    {
      return {.offset = 0, .size = 0};
    }

    return _samples[index];
  }

  std::span<std::byte const> Demuxer::samplePayload(std::uint32_t index) const
  {
    if (index >= _samples.size())
    {
      return {};
    }

    auto const& entry = _samples[index];

    if (entry.offset > _fileData.size() || entry.size > _fileData.size() - entry.offset)
    {
      return {};
    }

    return _fileData.subspan(entry.offset, entry.size);
  }

  std::uint32_t Demuxer::sampleIndexAtTime(std::uint64_t const time) const noexcept
  {
    if (_samples.empty())
    {
      return 0;
    }

    if (_samples.front().duration == 0)
    {
      if (_duration == 0)
      {
        return 0;
      }

      if (time >= _duration)
      {
        return sampleCount();
      }

      auto const scaledIndex = (static_cast<long double>(time) * static_cast<long double>(_samples.size())) /
                               static_cast<long double>(_duration);

      return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(scaledIndex), _samples.size()));
    }

    for (auto const& [index, sample] : std::ranges::views::enumerate(_samples))
    {
      if (time < sample.startTime + sample.duration)
      {
        return static_cast<std::uint32_t>(index);
      }
    }

    return sampleCount();
  }

  std::uint32_t Demuxer::timescale() const
  {
    return _timescale;
  }

  std::uint64_t Demuxer::duration() const
  {
    return _duration;
  }
} // namespace ao::media::mp4
