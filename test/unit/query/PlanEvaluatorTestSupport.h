// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/TestUtils.h"
#include "test/unit/library/TestUtils.h"
#include "test/unit/lmdb/TestUtils.h"
#include <ao/AudioCodec.h>
#include <ao/Type.h>
#include <ao/library/CoverArt.h>
#include <ao/library/DictionaryStore.h>
#include <ao/library/ResourceStore.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackLayout.h>
#include <ao/library/TrackView.h>
#include <ao/lmdb/Environment.h>
#include <ao/query/ExecutionPlan.h>
#include <ao/query/Expression.h>
#include <ao/query/Parser.h>
#include <ao/query/QueryCompiler.h>
#include <ao/utility/ByteView.h>

#include <catch2/catch_test_macros.hpp>
#include <lmdb.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::query::test
{
  using namespace ao::library;
  using namespace ao::library::test;
  using namespace ao::lmdb;
  using namespace ao::lmdb::test;

  inline Expression parseOk(std::string_view text)
  {
    auto result = ::ao::query::parse(text);
    REQUIRE(result.has_value());
    return std::move(*result);
  }

  inline ExecutionPlan compileOk(QueryCompiler& compiler, Expression const& expr)
  {
    auto result = compiler.compile(expr);
    REQUIRE(result.has_value());
    return std::move(*result);
  }

  inline ExecutionPlan compileOk(QueryCompiler&& compiler, Expression const& expr)
  {
    auto local = std::move(compiler);
    return compileOk(local, expr);
  }

  struct TrackSpec final
  {
    std::string title = "Test Title";
    std::string artist = "Test Artist";
    std::string album = "Test Album";
    std::string albumArtist = {};
    std::string composer = {};
    std::string work = {};
    std::string genre = {};
    std::string uri = "/path/to/track.flac";
    std::uint16_t year = 2020;
    std::uint16_t trackNumber = 1;
    std::uint16_t trackTotal = 0;
    std::uint16_t discNumber = 0;
    std::uint16_t discTotal = 0;
    std::chrono::milliseconds duration = std::chrono::seconds{180};
    std::uint32_t bitrate = 320000;
    std::uint32_t sampleRate = 44100;
    std::uint8_t channels = 2;
    std::uint8_t bitDepth = 16;
    ResourceId coverArtId{kInvalidResourceId};
    AudioCodec codec = AudioCodec::Unknown;
    std::uint32_t artistId = 0;
    std::uint32_t albumId = 0;
    std::uint32_t genreId = 0;
    std::uint32_t albumArtistId = 0;
    std::uint32_t composerId = 0;
    std::uint32_t workId = 0;
    std::vector<std::string> tags = {};
    std::vector<std::pair<std::string, std::string>> customPairs = {};
  };

  class TrackFixture final
  {
  public:
    TrackFixture() { init(TrackSpec{}, nullptr); }

    explicit TrackFixture(TrackSpec const& spec, DictionaryStore* dict = nullptr) { init(spec, dict); }

    TrackFixture(std::string title,
                 std::string artist = "Test Artist",
                 std::string album = "Test Album",
                 std::string uri = "/path/to/track.flac",
                 std::uint16_t year = 2020,
                 std::uint16_t trackNumber = 5,
                 std::uint32_t durationMillis = 180000,
                 std::uint32_t bitrate = 320000,
                 std::uint32_t sampleRate = 44100,
                 std::uint8_t channels = 2,
                 std::uint8_t bitDepth = 16,
                 std::uint32_t artistId = 0,
                 std::uint32_t albumId = 0,
                 std::uint32_t genreId = 0,
                 std::vector<std::uint32_t> const& tagIds = {},
                 std::string composer = "",
                 std::string work = "")
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
      init(spec, nullptr);
    }

    TrackView view() const { return TrackView{_hotData, _coldData}; }
    TrackView hotOnlyView() const { return TrackView{_hotData, std::span<std::byte const>{}}; }
    TrackView coldOnlyView() const { return TrackView{std::span<std::byte const>{}, _coldData}; }
    DictionaryStore& dictionary() { return *_optDict; }

  private:
    void init(TrackSpec const& spec, DictionaryStore* dict)
    {
      auto temp = ao::test::TempDir{};
      auto envOpts = Environment::Options{.flags = MDB_CREATE, .maxDatabases = 20};
      _optEnv.emplace(openEnvironment(temp.path(), envOpts));
      auto wtxn = beginWriteTransaction(*_optEnv);

      if (dict == nullptr)
      {
        _optDict.emplace(openDatabase(wtxn, "dict"), wtxn);
        dict = &*_optDict;
      }

      _optResources.emplace(openDatabase(wtxn, "resources"));

      TrackBuilder builder = TrackBuilder::createNew();
      builder.metadata().title(spec.title);
      builder.metadata().artist(spec.artist);
      builder.metadata().album(spec.album);
      builder.metadata().albumArtist(spec.albumArtist);
      builder.metadata().composer(spec.composer);
      builder.metadata().work(spec.work);
      builder.metadata().genre(spec.genre);
      builder.metadata().year(spec.year);
      builder.metadata().trackNumber(spec.trackNumber);
      builder.metadata().trackTotal(spec.trackTotal);
      builder.metadata().discNumber(spec.discNumber);
      builder.metadata().discTotal(spec.discTotal);

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

      for (auto const& [k, v] : spec.customPairs)
      {
        builder.customMetadata().add(k, v);
      }

      auto hotDataResult = builder.serializeHot(wtxn, *dict);
      REQUIRE(hotDataResult);
      auto coldDataResult = builder.serializeCold(wtxn, *dict, *_optResources);
      REQUIRE(coldDataResult);
      _hotData = *hotDataResult;
      _coldData = *coldDataResult;

      auto* header = utility::layout::asMutablePtr<library::TrackHotHeader>(_hotData);

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

      auto* coldHeader = utility::layout::asMutablePtr<library::TrackColdHeader>(_coldData);

      if (spec.workId != 0)
      {
        coldHeader->workId = DictionaryId{spec.workId};
      }
    }

    std::optional<Environment> _optEnv;
    std::optional<DictionaryStore> _optDict;
    std::optional<ResourceStore> _optResources;
    std::vector<std::byte> _hotData;
    std::vector<std::byte> _coldData;
  };

  using TestTrack = TrackFixture;

  inline std::vector<std::byte> makeHotOnlyTrack(DictionaryId artistId = kInvalidDictionaryId,
                                                 DictionaryId albumId = kInvalidDictionaryId,
                                                 DictionaryId genreId = kInvalidDictionaryId,
                                                 DictionaryId albumArtistId = kInvalidDictionaryId,
                                                 std::span<DictionaryId const> tagIds = {})
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
