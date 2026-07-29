// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::library
{
  class DictionaryStore;
  class TrackView;
  class WriteTransaction;

  namespace test
  {
  }
} // namespace ao::library

namespace ao::query
{
  struct ExecutionPlan;
  class PlanEvaluator;
} // namespace ao::query

namespace ao::query::test
{
  using namespace ao::library;
  using namespace ao::library::test;

  bool evaluateWithDictionary(PlanEvaluator const& evaluator,
                              ExecutionPlan const& plan,
                              TrackView const& track,
                              DictionaryStore const& dictionary);
  bool matchesWithDictionary(PlanEvaluator const& evaluator,
                             ExecutionPlan const& plan,
                             TrackView const& track,
                             DictionaryStore const& dictionary);

  class DictionaryFixture final
  {
  public:
    DictionaryFixture();
    ~DictionaryFixture();

    DictionaryFixture(DictionaryFixture const&) = delete;
    DictionaryFixture& operator=(DictionaryFixture const&) = delete;
    DictionaryFixture(DictionaryFixture&&) noexcept;
    DictionaryFixture& operator=(DictionaryFixture&&) noexcept;

    DictionaryStore const& dictionary();
    WriteTransaction writeTransaction();
    DictionaryId intern(std::string_view text);

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };

  struct TrackSpec final
  {
    std::string title = "Test Title";
    std::string artist = "Test Artist";
    std::string album = "Test Album";
    std::string albumArtist = {};
    std::string composer = {};
    std::string conductor = {};
    std::string ensemble = {};
    std::string work = {};
    std::string movement = {};
    std::string soloist = {};
    std::string genre = {};
    std::string uri = "/path/to/track.flac";
    std::uint16_t year = 2020;
    std::uint16_t trackNumber = 1;
    std::uint16_t trackTotal = 0;
    std::uint16_t discNumber = 0;
    std::uint16_t discTotal = 0;
    std::uint16_t movementNumber = 0;
    std::uint16_t movementTotal = 0;
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
    std::uint32_t conductorId = 0;
    std::uint32_t ensembleId = 0;
    std::uint32_t workId = 0;
    std::uint32_t movementId = 0;
    std::uint32_t soloistId = 0;
    std::vector<std::string> tags = {};
    std::vector<std::pair<std::string, std::string>> customPairs = {};
  };

  class TrackFixture final
  {
  public:
    TrackFixture();
    explicit TrackFixture(TrackSpec const& spec, DictionaryStore const* dictionary = nullptr);

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
                 std::string work = "");

    ~TrackFixture();

    TrackFixture(TrackFixture const&) = delete;
    TrackFixture& operator=(TrackFixture const&) = delete;
    TrackFixture(TrackFixture&&) noexcept;
    TrackFixture& operator=(TrackFixture&&) noexcept;

    TrackView view() const;
    TrackView hotOnlyView() const;
    TrackView coldOnlyView() const;
    DictionaryStore const& dictionary();

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };

  using TestTrack = TrackFixture;

  std::vector<std::byte> makeHotOnlyTrack(DictionaryId artistId = kInvalidDictionaryId,
                                          DictionaryId albumId = kInvalidDictionaryId,
                                          DictionaryId genreId = kInvalidDictionaryId,
                                          DictionaryId albumArtistId = kInvalidDictionaryId,
                                          std::span<DictionaryId const> tagIds = {});
} // namespace ao::query::test
