// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/media/mp4/Atom.h>
#include <rs/media/mp4/AtomLayout.h>
#include <rs/media/mp4/Demuxer.h>
#include <rs/utility/ByteView.h>

#include <algorithm>
#include <array>
#include <boost/endian/conversion.hpp>

namespace rs::media::mp4
{

  Demuxer::Demuxer(std::span<std::byte const> fileData)
    : _fileData(fileData)
  {
  }

  bool Demuxer::buildSampleOffsets(std::vector<SampleEntry>& samples,
                                   std::span<std::uint64_t const> chunkOffsets,
                                   std::span<SampleToChunkEntry const> sampleToChunk)
  {
    if (samples.empty() || chunkOffsets.empty() || sampleToChunk.empty())
    {
      return false;
    }

    auto sampleIndex = std::size_t{0};

    for (auto entryIndex = std::size_t{0}; entryIndex < sampleToChunk.size(); ++entryIndex)
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

      auto chunkEndIndex = chunkOffsets.size();

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

        for (auto sampleInChunk = std::uint32_t{0}; sampleInChunk < entry.samplesPerChunk; ++sampleInChunk)
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

  void Demuxer::parseStsz(std::span<std::byte const> bytes)
  {
    auto const* header = utility::layout::view<StszAtomLayout>(bytes);
    auto const sampleSize = header->sampleSize.value();
    auto const count = header->sampleCount.value();

    _samples.resize(count);

    if (sampleSize == 0)
    {
      auto entries = utility::layout::viewArray<StszAtomLayout::Entry>(bytes.subspan(sizeof(StszAtomLayout)));
      for (std::size_t i = 0; i < entries.size(); ++i)
      {
        _samples[i].size = entries[i].size.value();
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
    auto entries = utility::layout::viewArray<StscAtomLayout::Entry>(bytes.subspan(sizeof(StscAtomLayout)));

    for (std::size_t i = 0; i < entries.size(); ++i)
    {
      out[i].firstChunk = entries[i].firstChunk.value();
      out[i].samplesPerChunk = entries[i].samplesPerChunk.value();
    }
  }

  void Demuxer::parseStco(std::span<std::byte const> bytes, std::vector<std::uint64_t>& out)
  {
    auto const* header = utility::layout::view<StcoAtomLayout>(bytes);
    auto const count = header->entryCount.value();

    out.resize(count);
    auto entries = utility::layout::viewArray<StcoAtomLayout::Entry>(bytes.subspan(sizeof(StcoAtomLayout)));

    for (std::size_t i = 0; i < entries.size(); ++i)
    {
      out[i] = entries[i].chunkOffset.value();
    }
  }

  void Demuxer::parseCo64(std::span<std::byte const> bytes, std::vector<std::uint64_t>& out)
  {
    auto const* header = utility::layout::view<Co64AtomLayout>(bytes);
    auto const count = header->entryCount.value();

    out.resize(count);
    auto entries = utility::layout::viewArray<Co64AtomLayout::Entry>(bytes.subspan(sizeof(Co64AtomLayout)));

    for (std::size_t i = 0; i < entries.size(); ++i)
    {
      out[i] = entries[i].chunkOffset.value();
    }
  }

  std::string Demuxer::parseTrack(std::string_view targetFormat)
  {
    _magicCookie.clear();
    _samples.clear();
    _timescale = 0;
    _duration = 0;

    RootAtom root = fromBuffer(_fileData);
    auto chunkOffsets = std::vector<std::uint64_t>{};
    auto sampleToChunk = std::vector<SampleToChunkEntry>{};

    // mdhd path
    static constexpr std::array kMdhdPath = {
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"trak"},
      std::string_view{"mdia"},
      std::string_view{"mdhd"},
    };

    if (auto const* node = root.find(kMdhdPath))
    {
      auto const& view = rs::utility::unsafeDowncast<AtomView const>(*node);
      auto const& layout = view.layout<MdhdAtomLayout>();
      _timescale = layout.timescale.value();
      _duration = layout.duration.value();
    }

    // Cookie path (extensions in sample entry)
    std::array const kCookiePath = {
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

    if (auto const* node = root.find(kCookiePath))
    {
      auto const& view = rs::utility::unsafeDowncast<AtomView const>(*node);
      auto const bytes = view.bytes();
      _magicCookie.assign(bytes.begin(), bytes.end());
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
      return "Missing stbl atom";
    }

    stblNode->visitChildren(
      [this, &chunkOffsets, &sampleToChunk](Atom const& atom)
      {
        auto type = atom.type();
        auto const& view = rs::utility::unsafeDowncast<AtomView const>(atom);
        auto const atomBytes = view.bytes();

        if (type == "stsz")
        {
          parseStsz(atomBytes);
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
      return "Failed to extract track extradata or sample table";
    }

    if (!buildSampleOffsets(_samples, chunkOffsets, sampleToChunk))
    {
      return "Failed to resolve MP4 sample offsets";
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

  Demuxer::SampleEntry Demuxer::getSampleInfo(std::uint32_t index) const
  {
    if (index >= _samples.size())
    {
      return {0, 0};
    }
    return _samples[index];
  }

  std::span<std::byte const> Demuxer::getSamplePayload(std::uint32_t index) const
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

  std::uint32_t Demuxer::timescale() const
  {
    return _timescale;
  }

  std::uint64_t Demuxer::duration() const
  {
    return _duration;
  }

} // namespace rs::media::mp4
