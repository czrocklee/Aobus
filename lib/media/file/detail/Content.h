// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/PictureType.h>
#include <ao/media/file/Visitor.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::media::file::detail
{
  inline constexpr std::size_t kTextFieldCount = static_cast<std::size_t>(TextField::Soloist) + 1;
  inline constexpr std::size_t kNumberFieldCount = static_cast<std::size_t>(NumberField::MovementTotal) + 1;

  struct PictureView final
  {
    PictureType type = PictureType::FrontCover;
    std::span<std::byte const> bytes;
  };

  struct Content final
  {
    Content() = default;
    ~Content() = default;
    Content(Content&&) noexcept = default;
    Content& operator=(Content&&) noexcept = default;

    Content(Content const&) = delete;
    Content& operator=(Content const&) = delete;

    void visit(Visitor& visitor) const;

    std::array<std::string_view, kTextFieldCount> texts;
    std::array<std::uint16_t, kNumberFieldCount> numbers{};
    std::chrono::milliseconds duration{};
    Bitrate bitrate{};
    SampleRate sampleRate{};
    AudioCodec codec = AudioCodec::Unknown;
    Channels channels{};
    BitDepth bitDepth{};
    std::vector<PictureView> pictures;
    std::deque<std::string> ownedStrings;
  };

  class ContentBuilder final
  {
  public:
    static ContentBuilder makeEmpty();

    class MetadataBuilder final
    {
    public:
      MetadataBuilder& title(std::string_view value);
      MetadataBuilder& artist(std::string_view value);
      MetadataBuilder& album(std::string_view value);
      MetadataBuilder& albumArtist(std::string_view value);
      MetadataBuilder& composer(std::string_view value);
      MetadataBuilder& conductor(std::string_view value);
      MetadataBuilder& ensemble(std::string_view value);
      MetadataBuilder& genre(std::string_view value);
      MetadataBuilder& work(std::string_view value);
      MetadataBuilder& movement(std::string_view value);
      MetadataBuilder& soloist(std::string_view value);

      MetadataBuilder& year(std::uint16_t value);
      MetadataBuilder& trackNumber(std::uint16_t value);
      MetadataBuilder& trackTotal(std::uint16_t value);
      MetadataBuilder& discNumber(std::uint16_t value);
      MetadataBuilder& discTotal(std::uint16_t value);
      MetadataBuilder& movementNumber(std::uint16_t value);
      MetadataBuilder& movementTotal(std::uint16_t value);

      std::string_view title() const { return get(TextField::Title); }
      std::string_view artist() const { return get(TextField::Artist); }
      std::string_view album() const { return get(TextField::Album); }
      std::string_view albumArtist() const { return get(TextField::AlbumArtist); }
      std::string_view composer() const { return get(TextField::Composer); }
      std::string_view conductor() const { return get(TextField::Conductor); }
      std::string_view ensemble() const { return get(TextField::Ensemble); }
      std::string_view genre() const { return get(TextField::Genre); }
      std::string_view work() const { return get(TextField::Work); }
      std::string_view movement() const { return get(TextField::Movement); }
      std::string_view soloist() const { return get(TextField::Soloist); }

      std::uint16_t year() const { return get(NumberField::Year); }
      std::uint16_t trackNumber() const { return get(NumberField::TrackNumber); }
      std::uint16_t trackTotal() const { return get(NumberField::TrackTotal); }
      std::uint16_t discNumber() const { return get(NumberField::DiscNumber); }
      std::uint16_t discTotal() const { return get(NumberField::DiscTotal); }
      std::uint16_t movementNumber() const { return get(NumberField::MovementNumber); }
      std::uint16_t movementTotal() const { return get(NumberField::MovementTotal); }

    private:
      friend class ContentBuilder;

      explicit MetadataBuilder(Content& content);

      MetadataBuilder& text(TextField field, std::string_view value);
      MetadataBuilder& number(NumberField field, std::uint16_t value);

      std::string_view get(TextField field) const { return _content.texts[static_cast<std::size_t>(field)]; }
      std::uint16_t get(NumberField field) const { return _content.numbers[static_cast<std::size_t>(field)]; }

      Content& _content;
    };

    class PropertyBuilder final
    {
    public:
      PropertyBuilder& duration(std::chrono::milliseconds duration);
      PropertyBuilder& bitrate(Bitrate value);
      PropertyBuilder& sampleRate(SampleRate value);
      PropertyBuilder& codec(AudioCodec value);
      PropertyBuilder& channels(Channels value);
      PropertyBuilder& bitDepth(BitDepth value);

      std::chrono::milliseconds duration() const { return _content.duration; }
      Bitrate bitrate() const { return _content.bitrate; }
      SampleRate sampleRate() const { return _content.sampleRate; }
      AudioCodec codec() const { return _content.codec; }
      Channels channels() const { return _content.channels; }
      BitDepth bitDepth() const { return _content.bitDepth; }

    private:
      friend class ContentBuilder;

      explicit PropertyBuilder(Content& content);

      Content& _content;
    };

    class CoverArtBuilder final
    {
    public:
      CoverArtBuilder& add(PictureType type, std::span<std::byte const> bytes);

      std::vector<PictureView> const& entries() const { return _content.pictures; }

    private:
      friend class ContentBuilder;

      explicit CoverArtBuilder(Content& content);

      Content& _content;
    };

    ContentBuilder();

    ~ContentBuilder() = default;

    ContentBuilder(ContentBuilder&& other) noexcept;
    ContentBuilder& operator=(ContentBuilder&& other) noexcept;

    ContentBuilder(ContentBuilder const&) = delete;
    ContentBuilder& operator=(ContentBuilder const&) = delete;

    MetadataBuilder& metadata() { return _metadata; }
    MetadataBuilder const& metadata() const { return _metadata; }
    PropertyBuilder& property() { return _property; }
    PropertyBuilder const& property() const { return _property; }
    CoverArtBuilder& coverArt() { return _coverArt; }
    CoverArtBuilder const& coverArt() const { return _coverArt; }

    std::string_view own(std::string value);

    Content finish() &&;

  private:
    Content _content;
    MetadataBuilder _metadata;
    PropertyBuilder _property;
    CoverArtBuilder _coverArt;
  };
} // namespace ao::media::file::detail
