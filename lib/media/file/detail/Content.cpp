// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Content.h"

#include <ao/AudioCodec.h>
#include <ao/PictureType.h>
#include <ao/media/file/Visitor.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace ao::media::file::detail
{
  void Content::visit(Visitor& visitor) const
  {
    for (std::size_t index = 0; index < texts.size(); ++index)
    {
      if (auto const value = texts[index]; !value.empty())
      {
        visitor.text(static_cast<TextField>(index), value);
      }
    }

    for (std::size_t index = 0; index < numbers.size(); ++index)
    {
      if (auto const value = numbers[index]; value != 0)
      {
        visitor.number(static_cast<NumberField>(index), value);
      }
    }

    if (codec != AudioCodec::Unknown)
    {
      visitor.codec(codec);
    }

    if (duration > std::chrono::milliseconds{0})
    {
      visitor.duration(duration);
    }

    if (bitrate.raw() != 0)
    {
      visitor.bitrate(bitrate);
    }

    if (sampleRate.raw() != 0)
    {
      visitor.sampleRate(sampleRate);
    }

    if (channels.raw() != 0)
    {
      visitor.channels(channels);
    }

    if (bitDepth.raw() != 0)
    {
      visitor.bitDepth(bitDepth);
    }

    for (auto const& picture : pictures)
    {
      visitor.picture(picture.type, picture.bytes);
    }
  }

  ContentBuilder ContentBuilder::makeEmpty()
  {
    return {};
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::title(std::string_view value)
  {
    return text(TextField::Title, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::artist(std::string_view value)
  {
    return text(TextField::Artist, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::album(std::string_view value)
  {
    return text(TextField::Album, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::albumArtist(std::string_view value)
  {
    return text(TextField::AlbumArtist, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::composer(std::string_view value)
  {
    return text(TextField::Composer, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::conductor(std::string_view value)
  {
    return text(TextField::Conductor, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::ensemble(std::string_view value)
  {
    return text(TextField::Ensemble, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::genre(std::string_view value)
  {
    return text(TextField::Genre, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::work(std::string_view value)
  {
    return text(TextField::Work, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::movement(std::string_view value)
  {
    return text(TextField::Movement, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::soloist(std::string_view value)
  {
    return text(TextField::Soloist, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::year(std::uint16_t value)
  {
    return number(NumberField::Year, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::trackNumber(std::uint16_t value)
  {
    return number(NumberField::TrackNumber, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::trackTotal(std::uint16_t value)
  {
    return number(NumberField::TrackTotal, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::discNumber(std::uint16_t value)
  {
    return number(NumberField::DiscNumber, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::discTotal(std::uint16_t value)
  {
    return number(NumberField::DiscTotal, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::movementNumber(std::uint16_t value)
  {
    return number(NumberField::MovementNumber, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::movementTotal(std::uint16_t value)
  {
    return number(NumberField::MovementTotal, value);
  }

  ContentBuilder::MetadataBuilder::MetadataBuilder(Content& content)
    : _content{content}
  {
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::text(TextField field, std::string_view value)
  {
    _content.texts[static_cast<std::size_t>(field)] = value;
    return *this;
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::number(NumberField field, std::uint16_t value)
  {
    _content.numbers[static_cast<std::size_t>(field)] = value;
    return *this;
  }

  ContentBuilder::PropertyBuilder& ContentBuilder::PropertyBuilder::duration(std::chrono::milliseconds duration)
  {
    _content.duration = duration;
    return *this;
  }

  ContentBuilder::PropertyBuilder& ContentBuilder::PropertyBuilder::bitrate(Bitrate value)
  {
    _content.bitrate = value;
    return *this;
  }

  ContentBuilder::PropertyBuilder& ContentBuilder::PropertyBuilder::sampleRate(SampleRate value)
  {
    _content.sampleRate = value;
    return *this;
  }

  ContentBuilder::PropertyBuilder& ContentBuilder::PropertyBuilder::codec(AudioCodec value)
  {
    _content.codec = value;
    return *this;
  }

  ContentBuilder::PropertyBuilder& ContentBuilder::PropertyBuilder::channels(Channels value)
  {
    _content.channels = value;
    return *this;
  }

  ContentBuilder::PropertyBuilder& ContentBuilder::PropertyBuilder::bitDepth(BitDepth value)
  {
    _content.bitDepth = value;
    return *this;
  }

  ContentBuilder::PropertyBuilder::PropertyBuilder(Content& content)
    : _content{content}
  {
  }

  ContentBuilder::CoverArtBuilder& ContentBuilder::CoverArtBuilder::add(PictureType type,
                                                                        std::span<std::byte const> bytes)
  {
    if (!bytes.empty())
    {
      _content.pictures.push_back(PictureView{.type = type, .bytes = bytes});
    }

    return *this;
  }

  ContentBuilder::CoverArtBuilder::CoverArtBuilder(Content& content)
    : _content{content}
  {
  }

  ContentBuilder::ContentBuilder()
    : _metadata{_content}, _property{_content}, _coverArt{_content}
  {
  }

  ContentBuilder::ContentBuilder(ContentBuilder&& other) noexcept
    : _content{std::move(other._content)}, _metadata{_content}, _property{_content}, _coverArt{_content}
  {
  }

  ContentBuilder& ContentBuilder::operator=(ContentBuilder&& other) noexcept
  {
    _content = std::move(other._content);
    return *this;
  }

  std::string_view ContentBuilder::own(std::string value)
  {
    _content.ownedStrings.push_back(std::move(value));
    return _content.ownedStrings.back();
  }

  Content ContentBuilder::finish() &&
  {
    return std::move(_content);
  }
} // namespace ao::media::file::detail
