// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/AudioIdentityIndexer.h>
#include <ao/tag/TagFile.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::rt
{
  namespace
  {
    constexpr std::size_t kPendingIdentityBatchSize = 256;

    struct PendingIdentityRow final
    {
      std::string uri{};
      std::filesystem::path fullPath{};
      std::uint64_t fileSize = 0;
      std::uint64_t mtime = 0;
    };

    struct FileStatSnapshot final
    {
      std::uint64_t fileSize = 0;
      std::uint64_t mtime = 0;
    };

    std::uint64_t toManifestMtime(std::filesystem::file_time_type time)
    {
      return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
    }

    bool matchesSnapshot(FileStatSnapshot const& stat, PendingIdentityRow const& row) noexcept
    {
      return stat.fileSize == row.fileSize && stat.mtime == row.mtime;
    }

    std::optional<FileStatSnapshot> readCurrentStat(std::filesystem::path const& path, std::string& errorMessage)
    {
      auto ec = std::error_code{};
      auto const fileSize = std::filesystem::file_size(path, ec);

      if (ec)
      {
        errorMessage = ec.message();
        return std::nullopt;
      }

      auto const writeTime = std::filesystem::last_write_time(path, ec);

      if (ec)
      {
        errorMessage = ec.message();
        return std::nullopt;
      }

      return FileStatSnapshot{.fileSize = static_cast<std::uint64_t>(fileSize), .mtime = toManifestMtime(writeTime)};
    }

    std::vector<PendingIdentityRow> collectPendingRows(library::MusicLibrary& ml,
                                                       std::optional<std::string> const& optAfterUri)
    {
      auto rows = std::vector<PendingIdentityRow>{};
      auto const txn = ml.readTransaction();
      auto const reader = ml.manifest().reader(txn);

      for (auto const& [uriView, view] : reader)
      {
        auto uri = std::string{uriView};

        // Pagination relies on the manifest reader yielding URIs in strictly
        // increasing lexicographic byte order (LMDB's default memcmp
        // comparator for blob keys); a custom comparator would break it.
        if (optAfterUri && uri <= *optAfterUri)
        {
          continue;
        }

        if (view.status() != library::FileStatus::Available ||
            library::hasAudioIdentity(view.audioPayloadLength(), view.audioSignature()))
        {
          continue;
        }

        rows.push_back(PendingIdentityRow{.uri = uri,
                                          .fullPath = ml.rootPath() / std::filesystem::path{uri},
                                          .fileSize = view.fileSize(),
                                          .mtime = view.mtime()});

        if (rows.size() == kPendingIdentityBatchSize)
        {
          break;
        }
      }

      return rows;
    }

    bool pendingRowStillMatches(library::FileManifestView const& view, PendingIdentityRow const& row)
    {
      return view.status() == library::FileStatus::Available &&
             !library::hasAudioIdentity(view.audioPayloadLength(), view.audioSignature()) &&
             view.fileSize() == row.fileSize && view.mtime() == row.mtime;
    }

    struct HashedIdentityRow final
    {
      PendingIdentityRow const* row = nullptr;
      library::AudioIdentity identity{};
    };

    // Writes a batch of hashed identities in a single transaction so the
    // commit (and its fsync) is amortized across the batch instead of paid per
    // row. Each row is re-validated inside the transaction; a row that changed
    // since it was hashed is skipped without aborting the rest of the batch.
    // Counts are folded into the result only after the commit succeeds.
    Result<> writeHashedBatch(library::MusicLibrary& ml,
                              std::vector<HashedIdentityRow> const& hashedRows,
                              AudioIdentityIndexResult& result)
    {
      if (hashedRows.empty())
      {
        return {};
      }

      auto txn = ml.writeTransaction();
      auto writer = ml.manifest().writer(txn);
      std::int32_t batchCompletedCount = 0;
      std::int32_t batchSkippedCount = 0;

      for (auto const& hashed : hashedRows)
      {
        auto const& row = *hashed.row;
        auto currentResult = writer.get(row.uri);

        if (!currentResult)
        {
          if (currentResult.error().code == Error::Code::NotFound)
          {
            ++batchSkippedCount;
            continue;
          }

          return std::unexpected{currentResult.error()};
        }

        if (!pendingRowStillMatches(*currentResult, row))
        {
          ++batchSkippedCount;
          continue;
        }

        auto builder = library::FileManifestBuilder::fromView(*currentResult);
        builder.audioPayloadLength(hashed.identity.payloadLength).audioSignature(hashed.identity.signature);

        if (auto putResult = writer.put(row.uri, builder.serialize()); !putResult)
        {
          return std::unexpected{putResult.error()};
        }

        ++batchCompletedCount;
      }

      if (auto commitResult = txn.commit(); !commitResult)
      {
        return std::unexpected{commitResult.error()};
      }

      result.completedCount += batchCompletedCount;
      result.skippedCount += batchSkippedCount;
      return {};
    }

    void reportProgress(AudioIdentityIndexer::ProgressCallback& progressCallback,
                        std::filesystem::path const& path,
                        std::int32_t itemIndex,
                        double itemFraction)
    {
      if (progressCallback)
      {
        progressCallback(
          AudioIdentityIndexProgress{.path = path, .itemIndex = itemIndex, .itemFraction = itemFraction});
      }
    }

    void reportFailure(AudioIdentityIndexer::ItemFailureCallback& failureCallback,
                       std::string uri,
                       std::string stage,
                       std::string message)
    {
      if (failureCallback)
      {
        failureCallback(
          AudioIdentityIndexFailure{.uri = std::move(uri), .stage = std::move(stage), .message = std::move(message)});
      }
    }

    enum class RowOutcome : std::uint8_t
    {
      Hashed,
      Failed,
      Skipped,
      Cancelled
    };

    RowOutcome processPendingRow(PendingIdentityRow const& row,
                                 std::int32_t itemIndex,
                                 std::stop_token stopToken,
                                 AudioIdentityIndexer::ProgressCallback& progressCallback,
                                 AudioIdentityIndexer::ItemFailureCallback& failureCallback,
                                 std::vector<HashedIdentityRow>& hashedRows)
    {
      if (stopToken.stop_requested())
      {
        return RowOutcome::Cancelled;
      }

      auto ec = std::error_code{};
      auto const exists = std::filesystem::exists(row.fullPath, ec);

      if (ec)
      {
        reportFailure(failureCallback, row.uri, "stat", ec.message());
        return RowOutcome::Failed;
      }

      if (!exists)
      {
        return RowOutcome::Skipped;
      }

      auto const isRegularFile = std::filesystem::is_regular_file(row.fullPath, ec);

      if (ec)
      {
        reportFailure(failureCallback, row.uri, "stat", ec.message());
        return RowOutcome::Failed;
      }

      if (!isRegularFile || !tag::TagFile::isSupported(row.fullPath))
      {
        return RowOutcome::Skipped;
      }

      auto statError = std::string{};
      auto optBeforeHashStat = readCurrentStat(row.fullPath, statError);

      if (!optBeforeHashStat)
      {
        reportFailure(failureCallback, row.uri, "stat", statError);
        return RowOutcome::Failed;
      }

      if (!matchesSnapshot(*optBeforeHashStat, row))
      {
        return RowOutcome::Skipped;
      }

      auto identityResult = library::readAudioIdentity(
        row.fullPath,
        [&progressCallback, &row, itemIndex](double fraction)
        { reportProgress(progressCallback, row.fullPath, itemIndex, fraction); },
        stopToken);

      if (!identityResult)
      {
        reportFailure(failureCallback, row.uri, "fingerprint", identityResult.error().message);
        return RowOutcome::Failed;
      }

      if (!*identityResult)
      {
        return RowOutcome::Cancelled;
      }

      statError.clear();
      auto optAfterHashStat = readCurrentStat(row.fullPath, statError);

      if (!optAfterHashStat)
      {
        reportFailure(failureCallback, row.uri, "stat", statError);
        return RowOutcome::Failed;
      }

      if (!matchesSnapshot(*optAfterHashStat, row))
      {
        return RowOutcome::Skipped;
      }

      hashedRows.push_back(HashedIdentityRow{.row = &row, .identity = **identityResult});
      return RowOutcome::Hashed;
    }
  } // namespace

  AudioIdentityIndexer::AudioIdentityIndexer(library::MusicLibrary& library)
    : _library{library}
  {
  }

  Result<AudioIdentityIndexResult> AudioIdentityIndexer::indexPending(ProgressCallback progressCallback,
                                                                      ItemFailureCallback failureCallback,
                                                                      std::stop_token stopToken)
  {
    auto result = AudioIdentityIndexResult{};
    auto optAfterUri = std::optional<std::string>{};
    std::int32_t visitedCount = 0;

    while (true)
    {
      if (stopToken.stop_requested())
      {
        result.cancelled = true;
        return result;
      }

      auto rows = collectPendingRows(_library, optAfterUri);

      if (rows.empty())
      {
        return result;
      }

      optAfterUri = rows.back().uri;

      auto hashedRows = std::vector<HashedIdentityRow>{};
      hashedRows.reserve(rows.size());
      bool stopRequested = false;

      for (auto const& row : rows)
      {
        auto const itemIndex = visitedCount++;
        auto const outcome =
          processPendingRow(row, itemIndex, stopToken, progressCallback, failureCallback, hashedRows);

        if (outcome == RowOutcome::Cancelled)
        {
          stopRequested = true;
          break;
        }

        if (outcome == RowOutcome::Failed)
        {
          ++result.failureCount;
        }
        else if (outcome == RowOutcome::Skipped)
        {
          ++result.skippedCount;
        }
      }

      // Flush even on cancellation so already-computed hashes are not lost.
      if (auto writeResult = writeHashedBatch(_library, hashedRows, result); !writeResult)
      {
        return std::unexpected{writeResult.error()};
      }

      if (stopRequested)
      {
        result.cancelled = true;
        return result;
      }
    }
  }
} // namespace ao::rt
