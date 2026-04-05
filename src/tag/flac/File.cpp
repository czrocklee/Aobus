// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "../Decoder.h"
#include "MetadataBlock.h"
#include <rs/Exception.h>
#include <rs/tag/flac/File.h>
#include <rs/utility/ByteView.h>

#include <cstring>
#include <string>
#include <string_view>

namespace rs::tag::flac
{
  namespace
  {
    using Metadata = rs::core::TrackRecord::Metadata;

    void setText(std::string& field, std::string_view value)
    {
      field.assign(value);
    }

    void setNumber(std::uint16_t& field, std::string_view value)
    {
      if (auto parsed = decodeUint16(utility::asBytes(value)); parsed) { field = *parsed; }
    }

    void setSlashNumber(std::uint16_t& primary, std::uint16_t& secondary, std::string_view value)
    {
      auto const separator = value.find('/');
      setNumber(primary, value.substr(0, separator));

      if (separator != std::string_view::npos) { setNumber(secondary, value.substr(separator + 1)); }
    }

    template<auto Member>
    void assignTextField(Metadata& meta, std::string_view value)
    {
      setText(meta.*Member, value);
    }

    template<auto Member>
    void assignNumberField(Metadata& meta, std::string_view value)
    {
      setNumber(meta.*Member, value);
    }

    template<auto Primary, auto Secondary>
    void assignSlashField(Metadata& meta, std::string_view value)
    {
      setSlashNumber(meta.*Primary, meta.*Secondary, value);
    }

#include "tag/flac/VorbisCommentDispatch.h"

  } // namespace

  ParsedTrack File::loadTrack() const
  {
    if (_mappedRegion.get_size() < 4 || std::memcmp(_mappedRegion.get_address(), "fLaC", 4) != 0)
    {
      RS_THROW(rs::Exception, "unrecognized flac file content");
    }

    auto parsed = ParsedTrack{};
    auto iter = MetadataBlockViewIterator{
      static_cast<char const*>(_mappedRegion.get_address()) + 4, _mappedRegion.get_size() - 4};
    auto end = MetadataBlockViewIterator{};

    for (; iter != end; ++iter)
    {
      switch (iter->type())
      {
        case MetadataBlockType::StreamInfo:
        {
          auto view = StreamInfoBlockView{iter->data()};
          parsed.record.property.sampleRate = view.sampleRate();
          parsed.record.property.channels = view.channels();
          parsed.record.property.bitDepth = view.bitDepth();

          if (auto const totalSamples = view.totalSamples(); view.sampleRate() > 0 && totalSamples > 0)
          {
            parsed.record.property.durationMs = (totalSamples * 1000) / view.sampleRate();
          }

          break;
        }

        case MetadataBlockType::VorbisComment:
        {
          VorbisCommentBlockView{iter->data()}.visitComments([&](std::string_view comment) {
            auto const pos = comment.find('=');
            if (pos == std::string_view::npos) { return; }

            std::string_view key = comment.substr(0, pos);
            std::string_view value = comment.substr(pos + 1);

            if (auto const* entry = FlacVorbisDispatchTable::lookupVorbisField(key.data(), key.size()))
            {
              entry->handler(parsed.record.metadata, value);
            }
            else
            {
              parsed.record.custom.pairs.emplace_back(key, value);
            }
          });

          break;
        }

        case MetadataBlockType::Picture:
        {
          parsed.embeddedCoverArt = PictureBlockView{iter->data()}.blob();
          break;
        }

        default: break;
      }
    }

    return parsed;
  }
} // namespace rs::tag::flac
