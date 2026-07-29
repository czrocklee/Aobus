// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/query/PlanEvaluatorTestSupport.h"

#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/LibraryBinaryTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
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
#include <format>
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
                            std::uint8_t bitDepth,
                            std::uint32_t artistId,
                            std::uint32_t albumId,
                            std::uint32_t genreId,
                            std::vector<std::uint32_t> const& tagIds,
                            std::string composer,
                            std::string work)
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
      spec.artistId = artistId;
      spec.albumId = albumId;
      spec.genreId = genreId;

      for (auto id : tagIds)
      {
        spec.tags.push_back(std::format("tag{}", id));
      }

      spec.composer = std::move(composer);
      spec.work = std::move(work);
      return spec;
    }
  } // namespace

  bool evaluateWithDictionary(PlanEvaluator const& evaluator,
                              ExecutionPlan const& plan,
                              TrackView const& track,
                              DictionaryStore const& dictionary)
  {
    auto cache = DictionaryReadCache{dictionary};
    auto context = DictionaryReadContext{cache};
    auto const binding = PlanBinding{plan, context};
    return evaluator.evaluateFull(binding, track);
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
    MusicLibrary library{temp.path(), temp.path() / "db"};
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
    auto const id = ao::test::requireValue(transaction.dictionary().intern(text));
    REQUIRE(transaction.commit());
    return id;
  }

  struct TrackFixture::Impl final
  {
    explicit Impl(TrackSpec const& spec, DictionaryStore const* externalDictionary)
      : library{temp.path(), temp.path() / "db"}
      , transaction{library::test::writeTransaction(library)}
      , dictionary{externalDictionary != nullptr ? externalDictionary : &library.dictionary()}
    {
      setup(spec, externalDictionary);
    }

    void setup(TrackSpec const& spec, DictionaryStore const* externalDictionary)
    {
      auto builder = TrackBuilder::makeEmpty();
      builder.metadata().title(spec.title);
      builder.metadata().artist(spec.artist);
      builder.metadata().album(spec.album);
      builder.metadata().albumArtist(spec.albumArtist);
      builder.metadata().composer(spec.composer);
      builder.metadata().conductor(spec.conductor);
      builder.metadata().ensemble(spec.ensemble);
      builder.metadata().work(spec.work);
      builder.metadata().movement(spec.movement);
      builder.metadata().soloist(spec.soloist);
      builder.metadata().genre(spec.genre);
      builder.metadata().year(spec.year);
      builder.metadata().trackNumber(spec.trackNumber);
      builder.metadata().trackTotal(spec.trackTotal);
      builder.metadata().discNumber(spec.discNumber);
      builder.metadata().discTotal(spec.discTotal);
      builder.metadata().movementNumber(spec.movementNumber);
      builder.metadata().movementTotal(spec.movementTotal);

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

      auto hotDataResult = builder.serializeHot(transaction);
      REQUIRE(hotDataResult);
      auto coldDataResult = builder.serializeCold(transaction, library.resources());
      REQUIRE(coldDataResult);
      hotData = *hotDataResult;
      coldData = *coldDataResult;
      REQUIRE(transaction.commit());

      auto* header = utility::layout::asMutablePtr<library::TrackHotHeader>(hotData);

      if (externalDictionary != nullptr && !spec.tags.empty())
      {
        auto const tagByteCount = spec.tags.size() * sizeof(DictionaryId);
        auto tagBytes = std::span<std::byte>{hotData}.subspan(sizeof(library::TrackHotHeader), tagByteCount);
        auto tagIds = std::span<DictionaryId>{utility::layout::asMutablePtr<DictionaryId>(tagBytes), spec.tags.size()};
        header->tagBloom = 0;

        for (std::size_t index = 0; index < spec.tags.size(); ++index)
        {
          tagIds[index] = externalDictionary->lookupId(spec.tags[index]);
          header->tagBloom |= std::uint32_t{1} << (tagIds[index].raw() & 31U);
        }
      }

      if (spec.artistId != 0)
      {
        header->artistId = DictionaryId{spec.artistId};
      }

      if (spec.albumId != 0)
      {
        header->albumId = DictionaryId{spec.albumId};
      }

      if (spec.genreId != 0)
      {
        header->genreId = DictionaryId{spec.genreId};
      }

      if (spec.albumArtistId != 0)
      {
        header->albumArtistId = DictionaryId{spec.albumArtistId};
      }

      if (spec.composerId != 0)
      {
        header->composerId = DictionaryId{spec.composerId};
      }
    }

    // Declared before the LMDB-backed members so their mappings are closed
    // before the temporary directory is removed on Windows.
    ao::test::TempDir temp;
    MusicLibrary library;
    WriteTransaction transaction;
    DictionaryStore const* dictionary;
    std::vector<std::byte> hotData;
    std::vector<std::byte> coldData;
  };

  TrackFixture::TrackFixture()
    : TrackFixture{TrackSpec{}}
  {
  }

  TrackFixture::TrackFixture(TrackSpec const& spec, DictionaryStore const* dictionary)
    : _implPtr{std::make_unique<Impl>(spec, dictionary)}
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
                             std::uint8_t bitDepth,
                             std::uint32_t artistId,
                             std::uint32_t albumId,
                             std::uint32_t genreId,
                             std::vector<std::uint32_t> const& tagIds,
                             std::string composer,
                             std::string work)
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
                                 bitDepth,
                                 artistId,
                                 albumId,
                                 genreId,
                                 tagIds,
                                 std::move(composer),
                                 std::move(work))}
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
    return *_implPtr->dictionary;
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
    header.tagLength = static_cast<std::uint16_t>(tagIds.size_bytes());

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
