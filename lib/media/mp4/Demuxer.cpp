// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include <ao/Error.h>
#include <ao/media/mp4/Atom.h>
#include <ao/media/mp4/AtomLayout.h>
#include <ao/media/mp4/Demuxer.h>
#include <ao/utility/ByteView.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

        if ((value & 0x80U) == 0)
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

      if ((flags & 0x80U) != 0)
      {
        offset += 2;
      }

      if ((flags & 0x40U) != 0)
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

    std::optional<std::vector<std::byte>> findDescriptorPayload(std::span<std::byte const> bytes,
                                                                std::byte targetTag)
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
      auto const bytes = esdsView.bytes();

      constexpr std::size_t kFullAtomHeaderSize = sizeof(AtomLayout) + 4;

      if (bytes.size() <= kFullAtomHeaderSize)
      {
        return {};
      }

      auto const descriptors = bytes.subspan(kFullAtomHeaderSize);
      auto optCookie = findDescriptorPayload(descriptors, kDecoderSpecificInfoTag);
      return optCookie.value_or(std::vector<std::byte>{});
    }
  } // namespace

  Demuxer::Demuxer(std::span<std::byte const> fileData)
    : _fileData{fileData}
  {
  }

  bool Demuxer::applySampleTiming(std::vector<SampleEntry>& samples, std::span<TimeToSampleEntry const> timeToSample)
  {
    if (samples.empty() || timeToSample.empty())
    {
      return false;
    }

    std::size_t sampleIndex = 0;
    std::uint64_t sampleTime = 0;

    for (auto const& entry : timeToSample)
    {
      if (entry.sampleCount == 0 || entry.sampleDelta == 0)
      {
        return false;
      }

      for (std::uint32_t sample = 0; sample < entry.sampleCount; ++sample)
      {
        if (sampleIndex >= samples.size())
        {
          return false;
        }

        samples[sampleIndex].startTime = sampleTime;
        samples[sampleIndex].duration = entry.sampleDelta;
        sampleTime += entry.sampleDelta;
        ++sampleIndex;
      }
    }

    return sampleIndex == samples.size();
  }

  bool Demuxer::buildSampleOffsets(std::vector<SampleEntry>& samples,
                                   std::span<std::uint64_t const> chunkOffsets,
                                   std::span<SampleToChunkEntry const> sampleToChunk)
  {
    if (samples.empty() || chunkOffsets.empty() || sampleToChunk.empty())
    {
      return false;
    }

    std::size_t sampleIndex = 0;

    for (std::size_t entryIndex = 0; entryIndex < sampleToChunk.size(); ++entryIndex)
    {
      auto const& entry = sampleToChunk[entryIndex];

      if (entry.firstChunk == 0 || entry.samplesPerChunk == 0)
      {
        return false;
      }

      auto const chunkStartIndex = static_cast<std::size_t>(entry.firstChunk - 1);

      if (chunkStartIndex >= chunkOffsets.size())
      {
        return false;
      }

      std::size_t chunkEndIndex = chunkOffsets.size();

      if (entryIndex + 1 < sampleToChunk.size())
      {
        auto const nextFirstChunk = sampleToChunk[entryIndex + 1].firstChunk;

        if (nextFirstChunk <= entry.firstChunk)
        {
          return false;
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
            return false;
          }

          samples[sampleIndex].offset = sampleOffset;
          sampleOffset += samples[sampleIndex].size;
          ++sampleIndex;
        }
      }
    }

    return sampleIndex == samples.size();
  }

  void Demuxer::parseStts(std::span<std::byte const> bytes, std::vector<TimeToSampleEntry>& out)
  {
    auto const* header = utility::layout::view<SttsAtomLayout>(bytes);
    auto const count = header->entryCount.value();

    out.resize(count);
    auto const entries = utility::layout::viewArray<SttsAtomLayout::Entry>(bytes.subspan(sizeof(SttsAtomLayout)));

    for (auto const& [idx, entry] : std::ranges::views::enumerate(entries))
    {
      auto const uidx = static_cast<std::size_t>(idx);
      out[uidx].sampleCount = entry.sampleCount.value();
      out[uidx].sampleDelta = entry.sampleDelta.value();
    }
  }

  void Demuxer::parseStsz(std::span<std::byte const> bytes)
  {
    auto const* header = utility::layout::view<StszAtomLayout>(bytes);
    auto const sampleSize = header->sampleSize.value();
    auto const count = header->sampleCount.value();

    _samples.resize(count);

    if (sampleSize == 0)
    {
      auto const entries = utility::layout::viewArray<StszAtomLayout::Entry>(bytes.subspan(sizeof(StszAtomLayout)));

      for (auto const& [idx, entry] : std::ranges::views::enumerate(entries))
      {
        _samples[static_cast<std::size_t>(idx)].size = entry.size.value();
      }
    }
    else
    {
      for (auto& sample : _samples)
      {
        sample.size = sampleSize;
      }
    }
  }

  void Demuxer::parseStsc(std::span<std::byte const> bytes, std::vector<SampleToChunkEntry>& out)
  {
    auto const* header = utility::layout::view<StscAtomLayout>(bytes);
    auto const count = header->entryCount.value();

    out.resize(count);
    auto const entries = utility::layout::viewArray<StscAtomLayout::Entry>(bytes.subspan(sizeof(StscAtomLayout)));

    for (auto const& [idx, entry] : std::ranges::views::enumerate(entries))
    {
      auto const uidx = static_cast<std::size_t>(idx);
      out[uidx].firstChunk = entry.firstChunk.value();
      out[uidx].samplesPerChunk = entry.samplesPerChunk.value();
    }
  }

  void Demuxer::parseStco(std::span<std::byte const> bytes, std::vector<std::uint64_t>& out)
  {
    auto const* header = utility::layout::view<StcoAtomLayout>(bytes);
    auto const count = header->entryCount.value();

    out.resize(count);
    auto const entries = utility::layout::viewArray<StcoAtomLayout::Entry>(bytes.subspan(sizeof(StcoAtomLayout)));

    for (auto const& [idx, entry] : std::ranges::views::enumerate(entries))
    {
      out[static_cast<std::size_t>(idx)] = entry.chunkOffset.value();
    }
  }

  void Demuxer::parseCo64(std::span<std::byte const> bytes, std::vector<std::uint64_t>& out)
  {
    auto const* header = utility::layout::view<Co64AtomLayout>(bytes);
    auto const count = header->entryCount.value();

    out.resize(count);
    auto const entries = utility::layout::viewArray<Co64AtomLayout::Entry>(bytes.subspan(sizeof(Co64AtomLayout)));

    for (auto const& [idx, entry] : std::ranges::views::enumerate(entries))
    {
      out[static_cast<std::size_t>(idx)] = entry.chunkOffset.value();
    }
  }

  Result<> Demuxer::parseTrack(std::string_view targetFormat)
  {
    _magicCookie.clear();
    _samples.clear();
    _timescale = 0;
    _duration = 0;

    RootAtom const root = fromBuffer(_fileData);
    auto chunkOffsets = std::vector<std::uint64_t>{};
    auto sampleToChunk = std::vector<SampleToChunkEntry>{};
    auto timeToSample = std::vector<TimeToSampleEntry>{};

    // mdhd path
    static constexpr std::array kMdhdPath = {
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"trak"},
      std::string_view{"mdia"},
      std::string_view{"mdhd"},
    };

    if (auto const* node = root.find(kMdhdPath); node != nullptr)
    {
      auto const& view = utility::unsafeDowncast<AtomView const>(*node);
      auto const& layout = view.layout<MdhdAtomLayout>();
      _timescale = layout.timescale.value();
      _duration = layout.duration.value();
    }

    // Cookie path (extensions in sample entry)
    auto const kCookiePath = std::array{
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"trak"},
      std::string_view{"mdia"},
      std::string_view{"minf"},
      std::string_view{"stbl"},
      std::string_view{"stsd"},
      targetFormat,
      targetFormat,
    };

    if (auto const* node = root.find(kCookiePath); node != nullptr)
    {
      auto const& view = utility::unsafeDowncast<AtomView const>(*node);
      auto const bytes = view.bytes();
      _magicCookie.assign(bytes.begin(), bytes.end());
    }

    if (targetFormat == "mp4a")
    {
      static constexpr std::array kEsdsPath = {
        std::string_view{"root"},
        std::string_view{"moov"},
        std::string_view{"trak"},
        std::string_view{"mdia"},
        std::string_view{"minf"},
        std::string_view{"stbl"},
        std::string_view{"stsd"},
        std::string_view{"mp4a"},
        std::string_view{"esds"},
      };

      if (auto const* node = root.find(kEsdsPath); node != nullptr)
      {
        auto const& view = utility::unsafeDowncast<AtomView const>(*node);
        _magicCookie = extractAacMagicCookie(view);
      }
    }

    // stbl path
    static constexpr std::array kStblPath = {
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"trak"},
      std::string_view{"mdia"},
      std::string_view{"minf"},
      std::string_view{"stbl"},
    };

    auto const* stblNode = root.find(kStblPath);

    if (stblNode == nullptr)
    {
      return makeError(Error::Code::FormatRejected, "Missing stbl atom");
    }

    stblNode->visitChildren(
      [this, &chunkOffsets, &sampleToChunk, &timeToSample](Atom const& atom)
      {
        auto type = atom.type();
        auto const& view = utility::unsafeDowncast<AtomView const>(atom);

        if (auto const atomBytes = view.bytes(); type == "stsz")
        {
          parseStsz(atomBytes);
        }
        else if (type == "stts")
        {
          parseStts(atomBytes, timeToSample);
        }
        else if (type == "stsc")
        {
          parseStsc(atomBytes, sampleToChunk);
        }
        else if (type == "stco")
        {
          parseStco(atomBytes, chunkOffsets);
        }
        else if (type == "co64")
        {
          parseCo64(atomBytes, chunkOffsets);
        }

        return true;
      });

    if (_magicCookie.empty() || _samples.empty())
    {
      return makeError(Error::Code::FormatRejected, "Failed to extract track extradata or sample table");
    }

    if (!buildSampleOffsets(_samples, chunkOffsets, sampleToChunk))
    {
      return makeError(Error::Code::FormatRejected, "Failed to resolve MP4 sample offsets");
    }

    if (!timeToSample.empty() && !applySampleTiming(_samples, timeToSample))
    {
      return makeError(Error::Code::FormatRejected, "Failed to resolve MP4 sample timing");
    }

    return {};
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

    if (entry.offset + entry.size > _fileData.size())
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

    for (auto const& [idx, sample] : std::ranges::views::enumerate(_samples))
    {
      if (time < sample.startTime + sample.duration)
      {
        return static_cast<std::uint32_t>(idx);
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
