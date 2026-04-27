// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/media/mp4/Demuxer.h>
#include <rs/media/mp4/Atom.h>
#include <rs/media/mp4/AtomLayout.h>

#include <array>
#include <boost/endian/conversion.hpp>

namespace rs::media::mp4
{

  namespace
  {
    Atom const* findNode(Atom const& node, std::span<std::string_view const> path, std::size_t startPos = 0)
    {
      if (startPos >= path.size() || path[startPos] != node.type())
      {
        return nullptr;
      }

      if (startPos == path.size() - 1)
      {
        return &node;
      }

      Atom const* found = nullptr;
      node.visitChildren([&](auto const& child) { return ((found = findNode(child, path, startPos + 1)) == nullptr); });
      return found;
    }
  } // namespace

  Demuxer::Demuxer(std::span<std::byte const> fileData)
    : _fileData(fileData)
  {
  }

  std::string Demuxer::parseTrack(std::string_view targetFormat)
  {
    RootAtom root = fromBuffer(_fileData.data(), _fileData.size());

    // mdhd path
    static constexpr std::array kMdhdPath = {
      std::string_view{"root"},
      std::string_view{"moov"},
      std::string_view{"trak"},
      std::string_view{"mdia"},
      std::string_view{"mdhd"},
    };

    if (auto const* node = findNode(root, kMdhdPath))
    {
      auto const& view = static_cast<AtomView const&>(*node);
      auto const& layout = view.layout<MdhdAtomLayout>();
      _timescale = layout.timescale.value();
      _duration = layout.duration.value();
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

    auto const* stblNode = findNode(root, kStblPath);

    if (!stblNode)
    {
      return "Missing stbl atom";
    }

    stblNode->visitChildren(
      [this, targetFormat](Atom const& atom)
      {
        auto type = atom.type();
        auto const& view = static_cast<AtomView const&>(atom);

        if (type == "stsd")
        {
          constexpr std::size_t kStsdContentHeaderSize = 8;
          auto const* data = reinterpret_cast<std::uint8_t const*>(view.layout<AtomLayout>().type.data() + 4) +
                             kStsdContentHeaderSize;

          auto const entrySize = boost::endian::load_big_u32(data);
          auto const format = std::string_view{reinterpret_cast<char const*>(data + 4), 4};

          if (format == targetFormat)
          {
            auto const* extData = data + 8 + 28;
            auto const* extEnd = data + entrySize;

            while (extData + 8 <= extEnd)
            {
              auto const extSize = boost::endian::load_big_u32(extData);
              auto const extType = std::string_view{reinterpret_cast<char const*>(extData + 4), 4};

              if (extType == targetFormat)
              {
                _magicCookie.assign(reinterpret_cast<std::byte const*>(extData + 8),
                                    reinterpret_cast<std::byte const*>(extData + extSize));
                break;
              }

              if (extSize < 8)
              {
                break;
              }

              extData += extSize;
            }
          }
        }
        else if (type == "stsz")
        {
          auto const* data = reinterpret_cast<std::uint8_t const*>(view.layout<AtomLayout>().type.data() + 4) + 4;
          auto const sampleSize = boost::endian::load_big_u32(data);
          auto const count = boost::endian::load_big_u32(data + 4);

          _samples.resize(count);
          data += 8;

          for (std::uint32_t i = 0; i < count; ++i)
          {
            if (sampleSize == 0)
            {
              _samples[i].size = boost::endian::load_big_u32(data);
              data += 4;
            }
            else
            {
              _samples[i].size = sampleSize;
            }
          }
        }
        else if (type == "stco")
        {
          auto const* data = reinterpret_cast<std::uint8_t const*>(view.layout<AtomLayout>().type.data() + 4) + 4;
          auto const count = boost::endian::load_big_u32(data);

          data += 4;

          if (count == static_cast<std::uint32_t>(_samples.size()))
          {
            for (std::uint32_t i = 0; i < count; ++i)
            {
              _samples[i].offset = boost::endian::load_big_u32(data);
              data += 4;
            }
          }
        }

        return true;
      });

    if (_magicCookie.empty() || _samples.empty())
    {
      return "Failed to extract track extradata or sample table";
    }

    return {};
  }

  std::span<std::byte const> Demuxer::magicCookie() const { return _magicCookie; }

  std::uint32_t Demuxer::sampleCount() const { return static_cast<std::uint32_t>(_samples.size()); }

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

  std::uint32_t Demuxer::timescale() const { return _timescale; }

  std::uint64_t Demuxer::duration() const { return _duration; }

} // namespace rs::media::mp4