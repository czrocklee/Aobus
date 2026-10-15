// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "Content.h"

#include <ao/AudioCodec.h>
#include <ao/PictureType.h>
#include <ao/media/file/Visitor.h>
#include <ao/utility/String.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    // The ordered whole-list emission borrows this content's storage and
    // runs only once, after every scalar and property callback.
    if (!credits.empty())
    {
      visitor.visitCredits(credits);
    }
  }

  namespace
  {
    constexpr auto kJoinedValueSeparator = std::string_view{"; "};
  } // namespace

  ContentBuilder ContentBuilder::makeEmpty()
  {
    return {};
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::title(std::string_view value)
  {
    return replace(TextField::Title, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::artist(std::string_view value)
  {
    return join(TextField::Artist, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::album(std::string_view value)
  {
    return replace(TextField::Album, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::albumArtist(std::string_view value)
  {
    return join(TextField::AlbumArtist, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::composer(std::string_view value)
  {
    return join(TextField::Composer, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::conductor(std::string_view value)
  {
    return credit(value, CreditKind::Conductor);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::ensemble(std::string_view value)
  {
    return credit(value, CreditKind::Ensemble);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::genre(std::string_view value)
  {
    // A join would make combination sections such as "Rock; Pop", and taggers
    // write the most relevant genre first.
    if (auto& stored = _content.texts[static_cast<std::size_t>(TextField::Genre)];
        stored.empty() && !utility::trim(value).empty())
    {
      stored = value;
    }

    return *this;
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::replace(TextField field, std::string_view value)
  {
    auto const index = static_cast<std::size_t>(field);
    _content.texts[index] = value;
    // The replaced value starts the field over, so a later join compares
    // against it alone and extends a new text rather than the earlier one.
    _content.joinedTexts[index] = JoinedText{};
    return *this;
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::work(WorkSource source, std::string_view value)
  {
    // Blank after trimming surrounding ASCII whitespace: absent, it neither
    // wins nor blocks a lower-ranked source.
    if (utility::trim(value).empty())
    {
      return *this;
    }

    if (!_content.optWorkSource || source >= *_content.optWorkSource)
    {
      _content.texts[static_cast<std::size_t>(TextField::Work)] = value;
      _content.optWorkSource = source;
    }

    return *this;
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::movement(std::string_view value)
  {
    return replace(TextField::Movement, value);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::soloist(std::string_view value)
  {
    return credit(value, CreditKind::Soloist);
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::credit(std::string_view name,
                                                                           CreditKind kind,
                                                                           std::string_view role)
  {
    name = utility::trim(name);
    role = utility::trim(role);

    if (!name.empty())
    {
      _content.ownedStrings.emplace_back(name);
      auto const ownedName = std::string_view{_content.ownedStrings.back()};
      _content.ownedStrings.emplace_back(role);
      _content.credits.push_back({.name = ownedName, .kind = kind, .role = _content.ownedStrings.back()});
    }

    return *this;
  }

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::orchestra(std::string_view value)
  {
    value = utility::trim(value);

    if (!value.empty())
    {
      _content.ownedStrings.emplace_back(value);
      _content.orchestraCandidates.push_back(
        {.name = _content.ownedStrings.back(), .precedingCredits = _content.credits.size()});
    }

    return *this;
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

  ContentBuilder::MetadataBuilder& ContentBuilder::MetadataBuilder::join(TextField field, std::string_view value)
  {
    if (utility::trim(value).empty())
    {
      return *this;
    }

    auto const index = static_cast<std::size_t>(field);
    auto& stored = _content.texts[index];
    auto& joined = _content.joinedTexts[index];

    if (joined.values.empty() && !stored.empty())
    {
      joined.values.insert(stored);
    }

    // Whole source values decide repeats. The joined text cannot, because a
    // value may itself contain the separator.
    if (!joined.values.insert(value).second)
    {
      return *this;
    }

    if (stored.empty())
    {
      stored = value;
      return *this;
    }

    // One text per field, extended in place, keeps the retained bytes linear
    // in the source values however many there are.
    if (joined.text == nullptr)
    {
      joined.text = &_content.ownedStrings.emplace_back(stored);
    }

    joined.text->append(kJoinedValueSeparator).append(value);
    stored = *joined.text;
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
    if (!_content.orchestraCandidates.empty() &&
        !std::ranges::any_of(_content.credits, [](auto const& entry) { return entry.kind == CreditKind::Ensemble; }))
    {
      auto merged = std::vector<CreditView>{};
      merged.reserve(_content.credits.size() + _content.orchestraCandidates.size());
      std::size_t candidateIndex = 0;

      for (std::size_t creditIndex = 0; creditIndex <= _content.credits.size(); ++creditIndex)
      {
        while (candidateIndex < _content.orchestraCandidates.size() &&
               _content.orchestraCandidates[candidateIndex].precedingCredits == creditIndex)
        {
          merged.push_back({.name = _content.orchestraCandidates[candidateIndex++].name, .kind = CreditKind::Ensemble});
        }

        if (creditIndex < _content.credits.size())
        {
          merged.push_back(_content.credits[creditIndex]);
        }
      }

      _content.credits = std::move(merged);
    }

    _content.orchestraCandidates.clear();
    _content.joinedTexts = {};
    return std::move(_content);
  }
} // namespace ao::media::file::detail
