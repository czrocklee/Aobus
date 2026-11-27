// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/query/PlanEvaluatorTestSupport.h"

#include "test/TestFixtureSupport.h"
#include "test/library/LibraryBinaryTestSupport.h"
#include "test/library/MusicLibraryTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/PictureType.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/library/WriteTransaction.h>
#include <ao/query/PlanEvaluator.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::query::test
{
  namespace
  {
    TrackSpec makeTrackSpec(std::string title,
                            std::string artist,
                            std::string album,
                            std::string uri,
                            std::uint16_t year,
                            std::uint16_t trackNumber,
                            std::uint32_t durationMillis,
                            std::uint32_t bitrate,
                            std::uint32_t sampleRate,
                            std::uint8_t channels,
                            std::uint8_t bitDepth)
    {
      auto spec = TrackSpec{};
      spec.title = std::move(title);
      spec.artist = std::move(artist);
      spec.album = std::move(album);
      spec.uri = std::move(uri);
      spec.year = year;
      spec.trackNumber = trackNumber;
      spec.duration = std::chrono::milliseconds{durationMillis};
      spec.bitrate = bitrate;
      spec.sampleRate = sampleRate;
      spec.channels = channels;
      spec.bitDepth = bitDepth;
      return spec;
    }
  } // namespace

  bool matchesFullPlanWithDictionary(PlanEvaluator const& evaluator,
                                     ExecutionPlan const& plan,
                                     TrackView const& track,
                                     DictionaryStore const& dictionary)
  {
    auto cache = DictionaryReadCache{dictionary};
    auto context = DictionaryReadContext{cache};
    auto const binding = PlanBinding{plan, context};
    return evaluator.matchesFullPlan(binding, track);
  }

  bool matchesWithDictionary(PlanEvaluator const& evaluator,
                             ExecutionPlan const& plan,
                             TrackView const& track,
                             DictionaryStore const& dictionary)
  {
    auto cache = DictionaryReadCache{dictionary};
    auto context = DictionaryReadContext{cache};
    auto const binding = PlanBinding{plan, context};
    return evaluator.matches(binding, track);
  }

  struct DictionaryFixture::Impl final
  {
    // The library must be destroyed before its backing directory on Windows.
    ao::test::TempDir temp;
    MusicLibrary library{library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db")};
  };

  DictionaryFixture::DictionaryFixture()
    : _implPtr{std::make_unique<Impl>()}
  {
  }

  DictionaryFixture::~DictionaryFixture() = default;
  DictionaryFixture::DictionaryFixture(DictionaryFixture&&) noexcept = default;
  DictionaryFixture& DictionaryFixture::operator=(DictionaryFixture&&) noexcept = default;

  DictionaryStore const& DictionaryFixture::dictionary()
  {
    return _implPtr->library.dictionary();
  }

  WriteTransaction DictionaryFixture::writeTransaction()
  {
    return library::test::writeTransaction(_implPtr->library);
  }

  DictionaryId DictionaryFixture::intern(std::string_view text)
  {
    auto transaction = writeTransaction();
    auto const id = ao::test::requireValue(physicalDictionary(transaction).intern(text));
    REQUIRE(transaction.commit());
    return id;
  }

  struct TrackFixture::Impl final
  {
    explicit Impl(TrackSpec const& spec)
      : library{library::test::makeTestMusicLibrary(temp.path(), temp.path() / "db")}
    {
      auto transaction = library::test::writeTransaction(library);
      auto builder = TrackBuilder::makeEmpty();
      builder.metadata().title(spec.title);
      builder.metadata().artist(spec.artist);
      builder.metadata().album(spec.album);
      builder.metadata().albumArtist(spec.albumArtist);
      builder.metadata().composer(spec.composer);
      builder.metadata().work(spec.work);
      builder.metadata().movement(spec.movement);
      builder.metadata().genre(spec.genre);
      builder.metadata().year(spec.year);
      builder.metadata().trackNumber(spec.trackNumber);
      builder.metadata().trackTotal(spec.trackTotal);
      builder.metadata().discNumber(spec.discNumber);
      builder.metadata().discTotal(spec.discTotal);
      builder.metadata().movementNumber(spec.movementNumber);
      builder.metadata().movementTotal(spec.movementTotal);
      builder.metadata().recordingDate(spec.recordingDate);
      builder.metadata().credits(spec.credits);

      builder.property().uri(spec.uri);
      builder.property().duration(spec.duration);
      builder.property().bitrate(Bitrate{spec.bitrate});
      builder.property().sampleRate(SampleRate{spec.sampleRate});
      builder.property().channels(Channels{spec.channels});
      builder.property().bitDepth(BitDepth{spec.bitDepth});
      builder.property().codec(spec.codec);

      if (spec.coverArtId != kInvalidResourceId)
      {
        builder.coverArt().add(PictureType::FrontCover, spec.coverArtId);
      }

      for (auto const& name : spec.tags)
      {
        builder.tags().add(name);
      }

      for (auto const& [key, value] : spec.customPairs)
      {
        builder.customMetadata().add(key, value);
      }

      auto hotDataRes = library::test::physicalSerializeHotTrack(builder, transaction);
      REQUIRE(hotDataRes);
      auto coldDataRes = library::test::physicalSerializeColdTrack(builder, transaction, library.resources());
      REQUIRE(coldDataRes);
      hotData = std::move(*hotDataRes);
      coldData = std::move(*coldDataRes);
      REQUIRE(transaction.commit());
    }

    // Declared before the LMDB-backed members so their mappings are closed
    // before the temporary directory is removed on Windows.
    ao::test::TempDir temp;
    MusicLibrary library;
    std::vector<std::byte> hotData;
    std::vector<std::byte> coldData;
  };

  TrackFixture::TrackFixture()
    : TrackFixture{TrackSpec{}}
  {
  }

  TrackFixture::TrackFixture(TrackSpec const& spec)
    : _implPtr{std::make_unique<Impl>(spec)}
  {
  }

  TrackFixture::TrackFixture(std::string title,
                             std::string artist,
                             std::string album,
                             std::string uri,
                             std::uint16_t year,
                             std::uint16_t trackNumber,
                             std::uint32_t durationMillis,
                             std::uint32_t bitrate,
                             std::uint32_t sampleRate,
                             std::uint8_t channels,
                             std::uint8_t bitDepth)
    : TrackFixture{makeTrackSpec(std::move(title),
                                 std::move(artist),
                                 std::move(album),
                                 std::move(uri),
                                 year,
                                 trackNumber,
                                 durationMillis,
                                 bitrate,
                                 sampleRate,
                                 channels,
                                 bitDepth)}
  {
  }

  TrackFixture::~TrackFixture() = default;
  TrackFixture::TrackFixture(TrackFixture&&) noexcept = default;
  TrackFixture& TrackFixture::operator=(TrackFixture&&) noexcept = default;

  TrackView TrackFixture::view() const
  {
    return TrackView{_implPtr->hotData, _implPtr->coldData};
  }

  TrackView TrackFixture::hotOnlyView() const
  {
    return TrackView{_implPtr->hotData, std::span<std::byte const>{}};
  }

  TrackView TrackFixture::coldOnlyView() const
  {
    return TrackView{std::span<std::byte const>{}, _implPtr->coldData};
  }

  DictionaryStore const& TrackFixture::dictionary()
  {
    return _implPtr->library.dictionary();
  }

  std::vector<std::byte> makeHotOnlyTrack(DictionaryId artistId,
                                          DictionaryId albumId,
                                          DictionaryId genreId,
                                          DictionaryId albumArtistId,
                                          std::span<DictionaryId const> tagIds)
  {
    auto header = library::TrackHotHeader{};
    header.artistId = artistId;
    header.albumId = albumId;
    header.genreId = genreId;
    header.albumArtistId = albumArtistId;
    header.tagLengthAndFlags = library::packTrackHotTagLengthAndFlags(tagIds.size(), false);

    for (auto const tagId : tagIds)
    {
      header.tagBloom |= std::uint32_t{1} << (tagId.raw() & 31U);
    }

    auto data = serializeHeader(header);

    for (auto const tagId : tagIds)
    {
      data.insert_range(data.end(), utility::bytes::view(tagId));
    }

    appendString(data, "");
    return data;
  }
} // namespace ao::query::test
