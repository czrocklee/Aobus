// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Type.h>
#include <ao/library/AudioCodec.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/TrackView.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldReader.h>

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace ao::rt
{
  namespace
  {
    std::string resolve(library::DictionaryStore const& dict, DictionaryId id)
    {
      if (id.raw() == 0)
      {
        return {};
      }

      return std::string{dict.getOrDefault(id)};
    }
  } // namespace

  TrackFieldRawValue readTrackFieldRawValue(TrackField field,
                                            library::TrackView const& view,
                                            library::DictionaryStore const& dict,
                                            library::FileManifestStore::Reader const* manifestReader)
  {
    switch (field)
    {
      case TrackField::Title: return TrackFieldRawValue{std::in_place_type<std::string>, view.metadata().title()};
      case TrackField::Artist:
        return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dict, view.metadata().artistId())};
      case TrackField::Album:
        return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dict, view.metadata().albumId())};
      case TrackField::AlbumArtist:
        return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dict, view.metadata().albumArtistId())};
      case TrackField::Genre:
        return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dict, view.metadata().genreId())};
      case TrackField::Composer:
        return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dict, view.metadata().composerId())};
      case TrackField::Work:
        return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dict, view.metadata().workId())};

      case TrackField::Year: return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().year()};
      case TrackField::DiscNumber:
        return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().discNumber()};
      case TrackField::TotalDiscs:
        return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().totalDiscs()};
      case TrackField::TrackNumber:
        return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().trackNumber()};
      case TrackField::TotalTracks:
        return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().totalTracks()};

      case TrackField::Duration:
        return TrackFieldRawValue{std::in_place_type<TrackFieldDuration>, view.property().duration()};

      case TrackField::FilePath: return TrackFieldRawValue{std::in_place_type<std::string>, view.property().uri()};
      case TrackField::Codec:
      {
        return TrackFieldRawValue{
          std::in_place_type<std::string>, std::string{library::audioCodecName(view.property().codec())}};
      }
      case TrackField::SampleRate:
        return TrackFieldRawValue{std::in_place_type<std::uint32_t>, view.property().sampleRate().raw()};
      case TrackField::Channels:
        return TrackFieldRawValue{
          std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(view.property().channels().raw())};
      case TrackField::BitDepth:
        return TrackFieldRawValue{
          std::in_place_type<std::uint32_t>, static_cast<std::uint32_t>(view.property().bitDepth().raw())};
      case TrackField::Bitrate:
        return TrackFieldRawValue{std::in_place_type<std::uint32_t>, view.property().bitrate().raw()};

      case TrackField::FileSize:
      {
        if (manifestReader != nullptr)
        {
          if (auto const optManifestView = manifestReader->get(view.property().uri()); optManifestView)
          {
            return TrackFieldRawValue{std::in_place_type<std::uint64_t>, optManifestView->fileSize()};
          }
        }

        return std::monostate{};
      }

      case TrackField::ModifiedTime:
      {
        if (manifestReader != nullptr)
        {
          if (auto const optManifestView = manifestReader->get(view.property().uri()); optManifestView)
          {
            return TrackFieldRawValue{std::in_place_type<std::uint64_t>, optManifestView->mtime()};
          }
        }

        return std::monostate{};
      }

      case TrackField::Tags:
      case TrackField::DisplayTrackNumber:
      case TrackField::TechnicalSummary:
      case TrackField::Quality:
      default: return std::monostate{};
    }
  }
} // namespace ao::rt
