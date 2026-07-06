// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  struct AudioIdentityIndexProgress final
  {
    std::filesystem::path path{};
    std::int32_t itemIndex = 0;
    double itemFraction = 0.0;
  };

  struct AudioIdentityIndexResult final
  {
    std::int32_t completedCount = 0;
    std::int32_t skippedCount = 0;
    std::int32_t failureCount = 0;
    bool cancelled = false;
  };

  struct AudioIdentityIndexFailure final
  {
    std::string uri{};
    std::string stage{};
    std::string message{};
  };

  class AudioIdentityIndexer final
  {
  public:
    using ProgressCallback = std::move_only_function<void(AudioIdentityIndexProgress const& progress)>;
    using ItemFailureCallback = std::move_only_function<void(AudioIdentityIndexFailure const& failure)>;

    explicit AudioIdentityIndexer(library::MusicLibrary& library);
    ~AudioIdentityIndexer() = default;

    Result<AudioIdentityIndexResult> indexPending(ProgressCallback progressCallback = {},
                                                  ItemFailureCallback failureCallback = {},
                                                  std::stop_token stopToken = {});

    AudioIdentityIndexer(AudioIdentityIndexer const&) = delete;
    AudioIdentityIndexer& operator=(AudioIdentityIndexer const&) = delete;
    AudioIdentityIndexer(AudioIdentityIndexer&&) = delete;
    AudioIdentityIndexer& operator=(AudioIdentityIndexer&&) = delete;

  private:
    library::MusicLibrary& _library;
  };
} // namespace ao::rt
