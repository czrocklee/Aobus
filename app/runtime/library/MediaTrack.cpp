// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "MediaTrack.h"

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/Error.h>
#include <ao/PictureType.h>
#include <ao/library/TrackBuilder.h>
#include <ao/media/file/File.h>
#include <ao/media/file/Visitor.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>

namespace ao::rt
{
  namespace
  {
    class TrackBuilderVisitor final : public media::file::Visitor
    {
    public:
      explicit TrackBuilderVisitor(library::TrackBuilder& builder)
        : _builder{builder}
      {
      }

      void text(media::file::TextField field, std::string_view value) override
      {
        switch (auto& metadata = _builder.metadata(); field)
        {
          case media::file::TextField::Title: metadata.title(value); break;
          case media::file::TextField::Artist: metadata.artist(value); break;
          case media::file::TextField::Album: metadata.album(value); break;
          case media::file::TextField::AlbumArtist: metadata.albumArtist(value); break;
          case media::file::TextField::Composer: metadata.composer(value); break;
          case media::file::TextField::Conductor: metadata.conductor(value); break;
          case media::file::TextField::Ensemble: metadata.ensemble(value); break;
          case media::file::TextField::Genre: metadata.genre(value); break;
          case media::file::TextField::Work: metadata.work(value); break;
          case media::file::TextField::Movement: metadata.movement(value); break;
          case media::file::TextField::Soloist: metadata.soloist(value); break;
        }
      }

      void number(media::file::NumberField field, std::uint16_t value) override
      {
        switch (auto& metadata = _builder.metadata(); field)
        {
          case media::file::NumberField::Year: metadata.year(value); break;
          case media::file::NumberField::TrackNumber: metadata.trackNumber(value); break;
          case media::file::NumberField::TrackTotal: metadata.trackTotal(value); break;
          case media::file::NumberField::DiscNumber: metadata.discNumber(value); break;
          case media::file::NumberField::DiscTotal: metadata.discTotal(value); break;
          case media::file::NumberField::MovementNumber: metadata.movementNumber(value); break;
          case media::file::NumberField::MovementTotal: metadata.movementTotal(value); break;
        }
      }

      void codec(AudioCodec value) override { _builder.property().codec(value); }
      void duration(std::chrono::milliseconds duration) override { _builder.property().duration(duration); }
      void bitrate(Bitrate value) override { _builder.property().bitrate(value); }
      void sampleRate(SampleRate value) override { _builder.property().sampleRate(value); }
      void channels(Channels value) override { _builder.property().channels(value); }
      void bitDepth(BitDepth value) override { _builder.property().bitDepth(value); }

      void picture(PictureType type, std::span<std::byte const> bytes) override
      {
        _builder.coverArt().add(type, bytes);
      }

    private:
      library::TrackBuilder& _builder;
    };
  } // namespace

  MediaTrack::MediaTrack(media::file::File file, library::TrackBuilder builder)
    : _file{std::move(file)}, _builder{std::move(builder)}
  {
  }

  Result<MediaTrack> readMediaTrack(std::filesystem::path const& path)
  {
    auto fileResult = media::file::File::open(path);

    if (!fileResult)
    {
      return std::unexpected{fileResult.error()};
    }

    auto builder = library::TrackBuilder::makeEmpty();
    auto visitor = TrackBuilderVisitor{builder};

    if (auto const visitResult = fileResult->visit(visitor); !visitResult)
    {
      return std::unexpected{visitResult.error()};
    }

    return MediaTrack{std::move(*fileResult), std::move(builder)};
  }
} // namespace ao::rt
