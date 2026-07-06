// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/library/AudioIdentity.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

namespace ao::async
{
  class Runtime;
}

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  struct AudioIdentityIndexProgress final
  {
    /// File currently being fingerprinted.
    std::filesystem::path path{};
    /// Rows finished so far (completed, skipped, or failed). With concurrent
    /// fingerprinting this is the only monotonic notion of progress; there is
    /// no stable per-item ordering.
    std::int32_t processedCount = 0;
    /// Pending rows counted when indexing started. Best-effort snapshot: rows
    /// added while indexing runs are not included.
    std::int32_t totalCount = 0;
    /// Hash progress within `path`, in [0.0, 1.0].
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

  /// Same contract as library::readAudioIdentity(path, progress, stopToken).
  /// Invoked concurrently from multiple worker threads, so a replacement must
  /// be thread-safe.
  using AudioIdentityFingerprintFunction =
    std::function<Result<std::optional<library::AudioIdentity>>(std::filesystem::path const& path,
                                                                library::AudioIdentityProgressCallback progress,
                                                                std::stop_token stopToken)>;

  struct AudioIdentityIndexOptions final
  {
    /// Concurrent fingerprint workers per batch. 0 selects the default,
    /// clamp(hardware_concurrency / 2, 2, 4): fingerprinting is disk-bound
    /// on large files and shares the worker pool with UI tasks, so it must
    /// not saturate either.
    std::size_t maxConcurrency = 0;
    /// Test seam; defaults to library::readAudioIdentity.
    AudioIdentityFingerprintFunction fingerprint{};
  };

  class AudioIdentityIndexer final
  {
  public:
    /// Progress and failure callbacks are serialized by the indexer but may be
    /// invoked from any worker-pool thread, never concurrently.
    using ProgressCallback = std::move_only_function<void(AudioIdentityIndexProgress const& progress)>;
    using ItemFailureCallback = std::move_only_function<void(AudioIdentityIndexFailure const& failure)>;
    using FingerprintFunction = AudioIdentityFingerprintFunction;
    using Options = AudioIdentityIndexOptions;

    AudioIdentityIndexer(async::Runtime& asyncRuntime, library::MusicLibrary& library, std::mutex& mutationMutex);
    ~AudioIdentityIndexer() = default;

    /// Fill pending audio identities for available manifest rows. Rows are
    /// snapshotted in bounded batches without any lock, fingerprinted
    /// concurrently, then written back serially in one transaction per batch;
    /// every row is revalidated against the live manifest before its identity
    /// is committed. Cancellation flushes rows already hashed in the current
    /// batch and returns a cancelled result.
    async::Task<Result<AudioIdentityIndexResult>> indexPending(Options options = {},
                                                               ProgressCallback progressCallback = {},
                                                               ItemFailureCallback failureCallback = {},
                                                               std::stop_token stopToken = {});

    AudioIdentityIndexer(AudioIdentityIndexer const&) = delete;
    AudioIdentityIndexer& operator=(AudioIdentityIndexer const&) = delete;
    AudioIdentityIndexer(AudioIdentityIndexer&&) = delete;
    AudioIdentityIndexer& operator=(AudioIdentityIndexer&&) = delete;

  private:
    async::Runtime& _asyncRuntime;
    library::MusicLibrary& _library;
    std::mutex& _mutationMutex;
  };
} // namespace ao::rt
