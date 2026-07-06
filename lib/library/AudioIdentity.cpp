// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/library/AudioIdentity.h>
#include <ao/tag/TagFile.h>
#include <ao/utility/Xxh3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <utility>

namespace ao::library
{
  namespace
  {
    constexpr std::size_t kAudioIdentityChunkSize = 4ULL * 1024ULL * 1024ULL;
  }

  Result<std::optional<AudioIdentity>> readAudioIdentity(tag::TagFile const& tagFile,
                                                         AudioIdentityProgressCallback progress,
                                                         std::stop_token stopToken)
  {
    auto payloadResult = tagFile.audioPayload();

    if (!payloadResult)
    {
      return std::unexpected{payloadResult.error()};
    }

    auto const bytes = payloadResult->bytes;
    auto accumulator = utility::Xxh3Accumulator128{};
    std::size_t processed = 0;

    if (progress)
    {
      progress(0.0);
    }

    while (processed < bytes.size())
    {
      if (stopToken.stop_requested())
      {
        return std::optional<AudioIdentity>{};
      }

      auto const remaining = bytes.size() - processed;
      auto const chunkSize = std::min(kAudioIdentityChunkSize, remaining);
      accumulator.mix(bytes.subspan(processed, chunkSize));
      processed += chunkSize;

      if (progress)
      {
        auto const fraction = bytes.empty() ? 1.0 : static_cast<double>(processed) / static_cast<double>(bytes.size());
        progress(fraction);
      }

      if (stopToken.stop_requested())
      {
        return std::optional<AudioIdentity>{};
      }
    }

    if (bytes.empty() && progress)
    {
      progress(1.0);
    }

    return std::optional<AudioIdentity>{
      AudioIdentity{.signature = accumulator.value(), .payloadLength = static_cast<std::uint64_t>(bytes.size())}};
  }

  Result<std::optional<AudioIdentity>> readAudioIdentity(std::filesystem::path const& path,
                                                         AudioIdentityProgressCallback progress,
                                                         std::stop_token stopToken)
  {
    auto tagFileResult = tag::TagFile::open(path);

    if (!tagFileResult)
    {
      return std::unexpected{tagFileResult.error()};
    }

    return readAudioIdentity(**tagFileResult, std::move(progress), stopToken);
  }
} // namespace ao::library
