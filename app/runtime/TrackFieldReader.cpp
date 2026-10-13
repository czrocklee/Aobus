// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "runtime/TrackFieldReaderInternal.h"
#include <ao/AudioCodecText.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/FileTimestamp.h>
#include <ao/library/Credits.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/RecordingDate.h>
#include <ao/library/TrackView.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackFieldValue.h>

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace ao::rt
{
  namespace
  {
    std::string resolve(library::DictionaryStore const& dictionary, DictionaryId id)
    {
      if (id.raw() == 0)
      {
        return {};
      }

      return std::string{dictionary.getOrDefault(id)};
    }
  } // namespace

  TrackFieldRawValue readTrackFieldRawValue(TrackField field,
                                            library::TrackView const& view,
                                            library::DictionaryStore const& dictionary,
                                            library::FileManifestStore::Reader const* manifestReader)
  {
    auto const firstCreditName = [&](library::CreditKind kind)
    {
      auto const entries = view.performance().credits(kind);
      return entries.empty() ? std::string{} : resolve(dictionary, entries.front().nameId);
    };

    switch (field)
    {
      case TrackField::Title: return TrackFieldRawValue{std::in_place_type<std::string>, view.metadata().title()};
      case TrackField::Artist:
        return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dictionary, view.metadata().artistId())};
      case TrackField::Album:
        return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dictionary, view.metadata().albumId())};
      case TrackField::AlbumArtist:
        return TrackFieldRawValue{
          std::in_place_type<std::string>, resolve(dictionary, view.metadata().albumArtistId())};
      case TrackField::Genre:
        return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dictionary, view.metadata().genreId())};
      case TrackField::Composer:
        return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dictionary, view.metadata().composerId())};
      case TrackField::Conductor:
        return TrackFieldRawValue{std::in_place_type<std::string>, firstCreditName(library::CreditKind::Conductor)};
      case TrackField::Ensemble:
        return TrackFieldRawValue{std::in_place_type<std::string>, firstCreditName(library::CreditKind::Ensemble)};
      case TrackField::Work:
        return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dictionary, view.work().workId())};
      case TrackField::Movement:
        return TrackFieldRawValue{std::in_place_type<std::string>, resolve(dictionary, view.work().movementId())};
      case TrackField::Soloist:
        return TrackFieldRawValue{std::in_place_type<std::string>, firstCreditName(library::CreditKind::Soloist)};

      case TrackField::RecordingDate:
      {
        if (auto const date = view.performance().recordingDate(); date.isPresent())
        {
          return TrackFieldRawValue{std::in_place_type<library::RecordingDate>, date};
        }

        return std::monostate{};
      }

      case TrackField::Year: return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().year()};
      case TrackField::DiscNumber:
        return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().discNumber()};
      case TrackField::DiscTotal:
        return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().discTotal()};
      case TrackField::TrackNumber:
        return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().trackNumber()};
      case TrackField::TrackTotal:
        return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.metadata().trackTotal()};
      case TrackField::MovementNumber:
        return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.work().movementNumber()};
      case TrackField::MovementTotal:
        return TrackFieldRawValue{std::in_place_type<std::uint16_t>, view.work().movementTotal()};

      case TrackField::Duration:
        return TrackFieldRawValue{std::in_place_type<TrackFieldDuration>, view.property().duration()};

      case TrackField::FilePath: return TrackFieldRawValue{std::in_place_type<std::string>, view.property().uri()};
      case TrackField::Codec:
      {
        return TrackFieldRawValue{
          std::in_place_type<std::string>, std::string{audioCodecName(view.property().codec())}};
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
          auto const optManifest = manifestReader->get(view.property().uri());

          if (optManifest)
          {
            return TrackFieldRawValue{std::in_place_type<std::uint64_t>, optManifest->fileSize()};
          }
        }

        return std::monostate{};
      }

      case TrackField::ModifiedTime:
      {
        if (manifestReader != nullptr)
        {
          auto const optManifest = manifestReader->get(view.property().uri());

          if (optManifest)
          {
            if (auto const optMtime = optManifest->mtime(); optMtime)
            {
              return TrackFieldRawValue{std::in_place_type<FileTimestamp>, *optMtime};
            }
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
