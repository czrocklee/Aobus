// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/FileTimestamp.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/AudioIdentity.h>
#include <ao/rt/library/AudioIdentityIndex.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

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
  struct AudioIdentityWriteCandidate final
  {
    std::string uri{};
    std::uint64_t fileSize = 0;
    std::optional<FileTimestamp> optMtime;
    library::AudioIdentity identity{};
  };

  struct AudioIdentityBatchCommitResult final
  {
    std::int32_t completedCount = 0;
    std::int32_t skippedCount = 0;
  };

  /// Composes path opening and payload parsing with library::readAudioIdentity.
  /// Invoked concurrently from multiple worker threads, so a replacement must
  /// be thread-safe. Its Result reports the composition failures; an empty
  /// optional reports cooperative cancellation.
  using AudioIdentityFingerprintFunction =
    std::function<Result<std::optional<library::AudioIdentity>>(std::filesystem::path const& path,
                                                                library::AudioIdentityProgressCallback progress,
                                                                std::stop_token stopToken)>;

  struct AudioIdentityIndexOptions final
  {
    /// Requested concurrent fingerprint workers per batch. 0 selects the
    /// disk-bound default, clamp(hardware_concurrency / 2, 2, 4). Both explicit
    /// and default requests are capped at the Runtime worker count minus one
    /// (minimum one) to leave capacity for unrelated work where possible.
    std::size_t maxConcurrency = 0;
    /// Tests can replace the default file-open, payload-read, and fingerprint
    /// operation to control worker timing or force per-file failures.
    AudioIdentityFingerprintFunction fingerprint{};
  };

  class AudioIdentityIndexer final
  {
  public:
    using CommitBatchCallback = compat::MoveOnlyFunction<async::Task<Result<AudioIdentityBatchCommitResult>>(
      std::vector<AudioIdentityWriteCandidate> candidates)>;
    using FingerprintFunction = AudioIdentityFingerprintFunction;
    using Options = AudioIdentityIndexOptions;

    AudioIdentityIndexer(async::Runtime& asyncRuntime, library::MusicLibrary& library);
    ~AudioIdentityIndexer() = default;

    /// Fill pending audio identities for available manifest rows. Rows are
    /// snapshotted in bounded batches without any lock, fingerprinted
    /// concurrently, then written back serially in one transaction per batch;
    /// every row is revalidated against the live manifest before its identity
    /// is committed. On cooperative caller cancellation, completed hash
    /// candidates in the current batch are submitted for revalidation while
    /// the runtime and write lane remain live. A write error takes precedence
    /// over OperationCancelled. Terminal shutdown need not flush uncommitted work.
    /// Progress and failure callbacks may run on any worker-pool thread, but
    /// the indexer never invokes them concurrently.
    async::Task<Result<AudioIdentityIndexResult>> indexPendingAsync(
      CommitBatchCallback commitBatchCallback,
      Options options = {},
      AudioIdentityIndexProgressCallback progressCallback = {},
      AudioIdentityIndexFailureCallback failureCallback = {},
      std::stop_token stopToken = {});

    AudioIdentityIndexer(AudioIdentityIndexer const&) = delete;
    AudioIdentityIndexer& operator=(AudioIdentityIndexer const&) = delete;
    AudioIdentityIndexer(AudioIdentityIndexer&&) = delete;
    AudioIdentityIndexer& operator=(AudioIdentityIndexer&&) = delete;

  private:
    async::Runtime& _asyncRuntime;
    library::MusicLibrary& _library;
  };
} // namespace ao::rt
