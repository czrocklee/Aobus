// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/async/Parallel.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/AudioIdentityIndexer.h>
#include <ao/tag/TagFile.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
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

    std::int32_t countPendingRows(library::MusicLibrary& ml)
    {
      std::int32_t count = 0;
      auto const txn = ml.readTransaction();
      auto const reader = ml.manifest().reader(txn);

      for (auto const& [uriView, view] : reader)
      {
        if (view.status() == library::FileStatus::Available &&
            !library::hasAudioIdentity(view.audioPayloadLength(), view.audioSignature()))
        {
          ++count;
        }
      }

      return count;
    }

    bool pendingRowStillMatches(library::FileManifestView const& view, PendingIdentityRow const& row)
    {
      return view.status() == library::FileStatus::Available &&
             !library::hasAudioIdentity(view.audioPayloadLength(), view.audioSignature()) &&
             view.fileSize() == row.fileSize && view.mtime() == row.mtime;
    }

    std::size_t effectiveConcurrency(std::size_t requested)
    {
      if (requested != 0)
      {
        return requested;
      }

      auto const hardware = static_cast<std::size_t>(std::thread::hardware_concurrency());
      return std::clamp<std::size_t>(hardware / 2, 2, 4);
    }

    enum class RowOutcome : std::uint8_t
    {
      // Never pulled from the cursor, or hashing was interrupted by a stop
      // request; the row stays pending.
      NotProcessed,
      Hashed,
      Failed,
      Skipped
    };

    struct RowSlot final
    {
      RowOutcome outcome = RowOutcome::NotProcessed;
      library::AudioIdentity identity{};
    };

    // Shared state for one batch's fingerprint workers. Workers pull row
    // indices from `cursor` and each writes only its own `slots` entry, so the
    // vectors need no locking; user callbacks are serialized through
    // `callbackMutex`.
    struct FingerprintBatch final
    {
      std::vector<PendingIdentityRow> const& rows;
      std::vector<RowSlot>& slots;
      AudioIdentityIndexer::FingerprintFunction const& fingerprint;
      AudioIdentityIndexer::ProgressCallback& progressCallback;
      AudioIdentityIndexer::ItemFailureCallback& failureCallback;
      std::stop_token stopToken;
      std::int32_t totalCount = 0;
      std::atomic<std::size_t>& cursor;
      std::atomic<std::int32_t>& processedCount;
      std::mutex& callbackMutex;
    };

    void reportProgress(FingerprintBatch& batch, std::filesystem::path const& path, double itemFraction)
    {
      if (!batch.progressCallback)
      {
        return;
      }

      auto const lock = std::scoped_lock{batch.callbackMutex};
      batch.progressCallback(AudioIdentityIndexProgress{.path = path,
                                                        .processedCount = batch.processedCount.load(),
                                                        .totalCount = batch.totalCount,
                                                        .itemFraction = itemFraction});
    }

    void reportFailure(FingerprintBatch& batch, std::string uri, std::string stage, std::string message)
    {
      if (!batch.failureCallback)
      {
        return;
      }

      auto const lock = std::scoped_lock{batch.callbackMutex};
      batch.failureCallback(
        AudioIdentityIndexFailure{.uri = std::move(uri), .stage = std::move(stage), .message = std::move(message)});
    }

    RowSlot processPendingRow(FingerprintBatch& batch, PendingIdentityRow const& row)
    {
      auto ec = std::error_code{};
      auto const exists = std::filesystem::exists(row.fullPath, ec);

      if (ec)
      {
        reportFailure(batch, row.uri, "stat", ec.message());
        return RowSlot{.outcome = RowOutcome::Failed};
      }

      if (!exists)
      {
        return RowSlot{.outcome = RowOutcome::Skipped};
      }

      auto const isRegularFile = std::filesystem::is_regular_file(row.fullPath, ec);

      if (ec)
      {
        reportFailure(batch, row.uri, "stat", ec.message());
        return RowSlot{.outcome = RowOutcome::Failed};
      }

      if (!isRegularFile || !tag::TagFile::isSupported(row.fullPath))
      {
        return RowSlot{.outcome = RowOutcome::Skipped};
      }

      auto statError = std::string{};
      auto optBeforeHashStat = readCurrentStat(row.fullPath, statError);

      if (!optBeforeHashStat)
      {
        reportFailure(batch, row.uri, "stat", statError);
        return RowSlot{.outcome = RowOutcome::Failed};
      }

      if (!matchesSnapshot(*optBeforeHashStat, row))
      {
        return RowSlot{.outcome = RowOutcome::Skipped};
      }

      auto identityResult = batch.fingerprint(
        row.fullPath,
        [&batch, &row](double fraction) { reportProgress(batch, row.fullPath, fraction); },
        batch.stopToken);

      if (!identityResult)
      {
        reportFailure(batch, row.uri, "fingerprint", identityResult.error().message);
        return RowSlot{.outcome = RowOutcome::Failed};
      }

      if (!*identityResult)
      {
        return RowSlot{.outcome = RowOutcome::NotProcessed};
      }

      statError.clear();
      auto optAfterHashStat = readCurrentStat(row.fullPath, statError);

      if (!optAfterHashStat)
      {
        reportFailure(batch, row.uri, "stat", statError);
        return RowSlot{.outcome = RowOutcome::Failed};
      }

      if (!matchesSnapshot(*optAfterHashStat, row))
      {
        return RowSlot{.outcome = RowOutcome::Skipped};
      }

      return RowSlot{.outcome = RowOutcome::Hashed, .identity = **identityResult};
    }

    async::Task<> fingerprintWorker(FingerprintBatch* batch)
    {
      while (!batch->stopToken.stop_requested())
      {
        auto const index = batch->cursor.fetch_add(1);

        if (index >= batch->rows.size())
        {
          break;
        }

        auto const slot = processPendingRow(*batch, batch->rows[index]);

        if (slot.outcome != RowOutcome::NotProcessed)
        {
          batch->processedCount.fetch_add(1);
        }

        batch->slots[index] = slot;
      }

      co_return;
    }

    // Writes a batch of hashed identities in a single transaction so the
    // commit (and its fsync) is amortized across the batch instead of paid per
    // row. Each row is re-validated inside the transaction; a row that changed
    // since it was hashed is skipped without aborting the rest of the batch.
    // Counts are folded into the result only after the commit succeeds.
    Result<> writeHashedBatch(library::MusicLibrary& ml,
                              std::vector<PendingIdentityRow> const& rows,
                              std::vector<RowSlot> const& slots,
                              AudioIdentityIndexResult& result)
    {
      if (std::ranges::none_of(slots, [](RowSlot const& slot) { return slot.outcome == RowOutcome::Hashed; }))
      {
        return {};
      }

      auto txn = ml.writeTransaction();
      auto writer = ml.manifest().writer(txn);
      std::int32_t batchCompletedCount = 0;
      std::int32_t batchSkippedCount = 0;

      for (std::size_t index = 0; index < slots.size(); ++index)
      {
        if (slots[index].outcome != RowOutcome::Hashed)
        {
          continue;
        }

        auto const& row = rows[index];
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
        builder.audioPayloadLength(slots[index].identity.payloadLength).audioSignature(slots[index].identity.signature);

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
  } // namespace

  AudioIdentityIndexer::AudioIdentityIndexer(async::Runtime& asyncRuntime,
                                             library::MusicLibrary& library,
                                             std::mutex& mutationMutex)
    : _asyncRuntime{asyncRuntime}, _library{library}, _mutationMutex{mutationMutex}
  {
  }

  async::Task<Result<AudioIdentityIndexResult>> AudioIdentityIndexer::indexPending(Options options,
                                                                                   ProgressCallback progressCallback,
                                                                                   ItemFailureCallback failureCallback,
                                                                                   std::stop_token stopToken)
  {
    auto result = AudioIdentityIndexResult{};
    auto const fingerprint =
      options.fingerprint
        ? std::move(options.fingerprint)
        : FingerprintFunction{
            [](
              std::filesystem::path const& path, library::AudioIdentityProgressCallback progress, std::stop_token token)
            { return library::readAudioIdentity(path, std::move(progress), token); }};
    auto const concurrency = effectiveConcurrency(options.maxConcurrency);
    auto const totalCount = countPendingRows(_library);
    auto processedCount = std::atomic<std::int32_t>{0};
    auto callbackMutex = std::mutex{};
    auto optAfterUri = std::optional<std::string>{};

    while (true)
    {
      if (stopToken.stop_requested())
      {
        result.cancelled = true;
        co_return result;
      }

      // Phase 1: snapshot a batch of pending rows. A plain read transaction is
      // a consistent LMDB snapshot, and every row is revalidated before its
      // write, so no mutation lock is needed here.
      auto rows = collectPendingRows(_library, optAfterUri);

      if (rows.empty())
      {
        co_return result;
      }

      optAfterUri = rows.back().uri;

      // Phase 2: fingerprint the batch concurrently, without any lock or LMDB
      // transaction. The coordinator is suspended while workers run, so it
      // holds no worker-pool thread.
      auto slots = std::vector<RowSlot>(rows.size());
      auto cursor = std::atomic<std::size_t>{0};
      auto batch = FingerprintBatch{.rows = rows,
                                    .slots = slots,
                                    .fingerprint = fingerprint,
                                    .progressCallback = progressCallback,
                                    .failureCallback = failureCallback,
                                    .stopToken = stopToken,
                                    .totalCount = totalCount,
                                    .cursor = cursor,
                                    .processedCount = processedCount,
                                    .callbackMutex = callbackMutex};

      auto const workerCount = std::min(concurrency, rows.size());
      auto workers = std::vector<async::Task<>>{};
      workers.reserve(workerCount);

      for (std::size_t worker = 0; worker < workerCount; ++worker)
      {
        workers.push_back(fingerprintWorker(&batch));
      }

      co_await async::whenAll(&_asyncRuntime, std::move(workers));

      for (auto const& slot : slots)
      {
        if (slot.outcome == RowOutcome::Failed)
        {
          ++result.failureCount;
        }
        else if (slot.outcome == RowOutcome::Skipped)
        {
          ++result.skippedCount;
        }
      }

      // Phase 3: serial write-back under the mutation lock. Flush even on
      // cancellation so already-computed hashes are not lost.
      {
        auto mutationLock = std::scoped_lock{_mutationMutex};

        if (auto writeResult = writeHashedBatch(_library, rows, slots, result); !writeResult)
        {
          co_return std::unexpected{writeResult.error()};
        }
      }

      if (stopToken.stop_requested())
      {
        result.cancelled = true;
        co_return result;
      }
    }
  }
} // namespace ao::rt
