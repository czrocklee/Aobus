// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/runtime/library/ScanApplyTestSupport.h"

#include "runtime/library/ScanApplyOperation.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackStore.h>
#include <ao/media/flac/MetadataBlockLayout.h>
#include <ao/rt/library/LibraryScan.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  void replaceFile(std::filesystem::path const& target, std::filesystem::path const& source)
  {
    auto const previousTime = std::filesystem::last_write_time(target);
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::last_write_time(target, previousTime + std::chrono::seconds{10});
  }

  TrackId importOne(library::MusicLibrary& library)
  {
    auto plan = LibraryScan{library}.buildPlan().value();
    auto res = ScanApplyOperation{library, std::move(plan), {}, {}}.run();
    REQUIRE(res);
    REQUIRE(res->insertedIds.size() == 1);
    return res->insertedIds.front();
  }

  void requirePrepared(ScanApplyOperation& operation)
  {
    auto const prepareRes = operation.prepare();
    REQUIRE(prepareRes);
    REQUIRE(prepareRes->failureCount == 0);
  }

  void requireRevalidation(ScanApplyOperation& operation,
                           std::int32_t const staleCount,
                           std::int32_t const failureCount)
  {
    auto const revalidationRes = operation.revalidatePreparedFiles();
    REQUIRE(revalidationRes);
    REQUIRE(revalidationRes->staleCount == staleCount);
    REQUIRE(revalidationRes->failureCount == failureCount);
    REQUIRE(operation.isReadyForMutation() == (failureCount == 0));
  }

  void writeScanFlacMetadataFixture(std::filesystem::path const& path,
                                    std::vector<std::string> const& comments,
                                    std::uint64_t const totalSamples)
  {
    constexpr std::uint64_t kSampleRate = 44100;

    auto data = std::vector<std::uint8_t>{'f', 'L', 'a', 'C'};

    auto appendBlockHeader = [&data](
                               media::flac::MetadataBlockType const type, bool const isLast, std::uint32_t const size)
    {
      auto first = static_cast<std::uint8_t>(type);

      if (isLast)
      {
        first |= 0x80;
      }

      data.push_back(first);
      data.push_back((size >> 16) & 0xFF);
      data.push_back((size >> 8) & 0xFF);
      data.push_back(size & 0xFF);
    };

    appendBlockHeader(media::flac::MetadataBlockType::StreamInfo, false, media::flac::StreamInfoLayout::kSize);
    auto streamInfo = media::flac::StreamInfoLayout{};
    // sampleRate(20) | channels-1(3) | bits-1(5) | totalSamples(36)
    streamInfo.packedFields = (kSampleRate << 44) | (1ULL << 41) | (15ULL << 36) | totalSamples;
    auto const* streamInfoBytes = reinterpret_cast<std::uint8_t const*>(&streamInfo);
    data.insert(data.end(), streamInfoBytes, streamInfoBytes + media::flac::StreamInfoLayout::kSize);

    auto commentBlock = std::vector<std::uint8_t>{};
    auto const appendString = [&commentBlock](std::string_view const text)
    {
      auto const length = static_cast<std::uint32_t>(text.size());
      commentBlock.push_back(length & 0xFF);
      commentBlock.push_back((length >> 8) & 0xFF);
      commentBlock.push_back((length >> 16) & 0xFF);
      commentBlock.push_back((length >> 24) & 0xFF);
      commentBlock.insert(commentBlock.end(), text.begin(), text.end());
    };

    appendString("AobusScanMetadataFixture");
    auto const count = static_cast<std::uint32_t>(comments.size());
    commentBlock.push_back(count & 0xFF);
    commentBlock.push_back((count >> 8) & 0xFF);
    commentBlock.push_back((count >> 16) & 0xFF);
    commentBlock.push_back((count >> 24) & 0xFF);

    for (auto const& comment : comments)
    {
      appendString(comment);
    }

    appendBlockHeader(
      media::flac::MetadataBlockType::VorbisComment, true, static_cast<std::uint32_t>(commentBlock.size()));
    data.insert(data.end(), commentBlock.begin(), commentBlock.end());

    // The scanner rejects a FLAC file without an audio payload.
    data.push_back(0xA0);
    data.push_back(0x24);

    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<char const*>(data.data()), static_cast<std::streamsize>(data.size()));
    REQUIRE(output.good());
  }

  library::test::TrackSpec storedTrackSpec(library::MusicLibrary& library, TrackId const trackId)
  {
    auto transaction = library.readTransaction();
    auto const optView = library.tracks().reader(transaction).get(trackId, library::TrackStore::Reader::LoadMode::Both);
    REQUIRE(optView);
    return library::test::trackSpecFromView(library, *optView);
  }
} // namespace ao::rt::test
