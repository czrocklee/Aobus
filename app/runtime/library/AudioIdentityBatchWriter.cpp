// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AudioIdentityBatchWriter.h"

#include <ao/Error.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/FileManifestStore.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/WriteTransaction.h>
#include <ao/rt/library/AudioIdentityIndexer.h>

#include <expected>
#include <span>

namespace ao::rt
{
  namespace
  {
    bool matchesCandidate(library::FileManifestView const& view, AudioIdentityWriteCandidate const& candidate) noexcept
    {
      return view.status() == library::FileStatus::Available &&
             !library::hasAudioIdentity(view.audioPayloadLength(), view.audioSignature()) &&
             view.fileSize() == candidate.fileSize && view.mtime() == candidate.mtime;
    }
  } // namespace

  Result<AudioIdentityBatchCommitResult> applyAudioIdentityBatch(
    library::MusicLibrary& library,
    library::WriteTransaction& transaction,
    std::span<AudioIdentityWriteCandidate const> candidates)
  {
    auto writer = library.manifest().writer(transaction);
    auto result = AudioIdentityBatchCommitResult{};

    for (auto const& candidate : candidates)
    {
      auto currentResult = writer.get(candidate.uri);

      if (!currentResult)
      {
        if (currentResult.error().code == Error::Code::NotFound)
        {
          ++result.skippedCount;
          continue;
        }

        return std::unexpected{currentResult.error()};
      }

      if (!matchesCandidate(*currentResult, candidate))
      {
        ++result.skippedCount;
        continue;
      }

      auto builder = library::FileManifestBuilder::fromView(*currentResult);
      builder.audioPayloadLength(candidate.identity.payloadLength).audioSignature(candidate.identity.signature);

      if (auto putResult = writer.put(candidate.uri, builder.serialize()); !putResult)
      {
        return std::unexpected{putResult.error()};
      }

      ++result.completedCount;
    }

    return result;
  }
} // namespace ao::rt
