// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/library/CoverArt.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/library/TrackView.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library::test
{
  struct TrackSpec final
  {
    std::string title = "Track";
    std::string artist = "Artist";
    std::string album = "Album";
    std::string albumArtist{};
    std::string genre{};
    std::string composer{};
    std::string conductor{};
    std::string ensemble{};
    std::string work{};
    std::string movement{};
    std::string soloist{};
    std::string uri = "/tmp/test.flac";
    std::vector<std::string> tags{};
    std::vector<std::pair<std::string, std::string>> customMetadata{};
    ResourceId coverArtId = kInvalidResourceId;
    std::uint16_t year = 2020;
    std::uint16_t discNumber = 1;
    std::uint16_t discTotal = 0;
    std::uint16_t trackNumber = 1;
    std::uint16_t trackTotal = 0;
    std::uint16_t movementNumber = 0;
    std::uint16_t movementTotal = 0;
    std::chrono::milliseconds duration = std::chrono::seconds{200};
    Bitrate bitrate = Bitrate{320000};
    SampleRate sampleRate = SampleRate{44100};
    Channels channels = Channels{2};
    BitDepth bitDepth = BitDepth{16};
    AudioCodec codec = AudioCodec::Unknown;
  };

  inline TrackSpec makeTrackSpec(std::string_view title, std::uint16_t year = 2020)
  {
    return TrackSpec{.title = std::string{title}, .year = year};
  }

  inline TrackSpec makeEmptyTrackSpec(std::string_view uri)
  {
    return TrackSpec{.title = "",
                     .artist = "",
                     .album = "",
                     .uri = std::string{uri},
                     .year = 0,
                     .discNumber = 0,
                     .trackNumber = 0,
                     .duration = std::chrono::milliseconds{0},
                     .bitrate = Bitrate{},
                     .sampleRate = SampleRate{},
                     .channels = Channels{},
                     .bitDepth = BitDepth{}};
  }

  inline void applyTrackSpec(TrackBuilder& builder, TrackSpec const& spec)
  {
    builder.metadata()
      .title(spec.title)
      .artist(spec.artist)
      .album(spec.album)
      .albumArtist(spec.albumArtist)
      .genre(spec.genre)
      .composer(spec.composer)
      .conductor(spec.conductor)
      .ensemble(spec.ensemble)
      .work(spec.work)
      .movement(spec.movement)
      .soloist(spec.soloist)
      .year(spec.year)
      .discNumber(spec.discNumber)
      .discTotal(spec.discTotal)
      .trackNumber(spec.trackNumber)
      .trackTotal(spec.trackTotal)
      .movementNumber(spec.movementNumber)
      .movementTotal(spec.movementTotal);
    builder.property()
      .uri(spec.uri)
      .duration(spec.duration)
      .bitrate(spec.bitrate)
      .sampleRate(spec.sampleRate)
      .codec(spec.codec)
      .channels(spec.channels)
      .bitDepth(spec.bitDepth);

    builder.tags().clear();

    for (auto const& tag : spec.tags)
    {
      builder.tags().add(tag);
    }

    builder.customMetadata().clear();

    for (auto const& [key, value] : spec.customMetadata)
    {
      builder.customMetadata().add(key, value);
    }

    builder.coverArt().clear();

    if (spec.coverArtId != kInvalidResourceId)
    {
      builder.coverArt().add(PictureType::FrontCover, spec.coverArtId);
    }
  }

  inline TrackSpec trackSpecFromView(MusicLibrary const& library, TrackView const& view)
  {
    auto spec =
      TrackSpec{.title = std::string{view.metadata().title()},
                .artist = std::string{library.dictionary().getOrDefault(view.metadata().artistId())},
                .album = std::string{library.dictionary().getOrDefault(view.metadata().albumId())},
                .albumArtist = std::string{library.dictionary().getOrDefault(view.metadata().albumArtistId())},
                .genre = std::string{library.dictionary().getOrDefault(view.metadata().genreId())},
                .composer = std::string{library.dictionary().getOrDefault(view.metadata().composerId())},
                .conductor = std::string{library.dictionary().getOrDefault(view.classical().conductorId())},
                .ensemble = std::string{library.dictionary().getOrDefault(view.classical().ensembleId())},
                .work = std::string{library.dictionary().getOrDefault(view.classical().workId())},
                .movement = std::string{library.dictionary().getOrDefault(view.classical().movementId())},
                .soloist = std::string{library.dictionary().getOrDefault(view.classical().soloistId())},
                .uri = std::string{view.property().uri()},
                .year = view.metadata().year(),
                .discNumber = view.metadata().discNumber(),
                .discTotal = view.metadata().discTotal(),
                .trackNumber = view.metadata().trackNumber(),
                .trackTotal = view.metadata().trackTotal(),
                .movementNumber = view.classical().movementNumber(),
                .movementTotal = view.classical().movementTotal(),
                .duration = view.property().duration(),
                .bitrate = view.property().bitrate(),
                .sampleRate = view.property().sampleRate(),
                .channels = view.property().channels(),
                .bitDepth = view.property().bitDepth(),
                .codec = view.property().codec()};

    for (auto const tagId : view.tags())
    {
      spec.tags.emplace_back(library.dictionary().getOrDefault(tagId));
    }

    for (auto const [keyId, value] : view.customMetadata())
    {
      spec.customMetadata.emplace_back(std::string{library.dictionary().getOrDefault(keyId)}, std::string{value});
    }

    if (auto const optCover = view.coverArt().primary(); optCover)
    {
      spec.coverArtId = optCover->resourceId;
    }

    return spec;
  }

  inline TrackId addTrack(MusicLibrary& library, TrackSpec const& spec)
  {
    auto transaction = library.writeTransaction();
    auto writer = library.tracks().writer(transaction);
    auto builder = TrackBuilder::makeEmpty();
    applyTrackSpec(builder, spec);

    auto data = builder.serialize(transaction, library.dictionary(), library.resources());
    REQUIRE(data);
    auto createResult = writer.createHotCold(data->first, data->second);
    REQUIRE(createResult);
    auto const [id, _] = *createResult;
    REQUIRE(transaction.commit());
    return id;
  }

  inline void mutateTrack(MusicLibrary& library, TrackId id, std::move_only_function<void(TrackBuilder&)> mutate)
  {
    auto transaction = library.writeTransaction();
    auto reader = library.tracks().reader(transaction);
    auto writer = library.tracks().writer(transaction);
    auto optView = reader.get(id, TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);

    auto builder = TrackBuilder::fromView(*optView, library.dictionary());
    mutate(builder);

    auto hotData = builder.serializeHot(transaction, library.dictionary());
    REQUIRE(hotData);
    auto coldData = builder.serializeCold(transaction, library.dictionary(), library.resources());
    REQUIRE(coldData);
    REQUIRE(writer.updateHot(id, *hotData));
    REQUIRE(writer.updateCold(
      id, coldData->size(), [&](std::span<std::byte> buf) { std::ranges::copy(*coldData, buf.begin()); }));
    REQUIRE(transaction.commit());
  }

  inline void updateTrackSpec(MusicLibrary& library, TrackId id, std::move_only_function<void(TrackSpec&)> updater)
  {
    auto spec = TrackSpec{};
    {
      auto transaction = library.readTransaction();
      auto reader = library.tracks().reader(transaction);
      auto optView = reader.get(id, TrackStore::Reader::LoadMode::Both);
      REQUIRE(optView);
      spec = trackSpecFromView(library, *optView);
    }

    updater(spec);
    mutateTrack(library, id, [&](TrackBuilder& builder) { applyTrackSpec(builder, spec); });
  }
} // namespace ao::library::test
