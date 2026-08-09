// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "AudioIdentityBatchWriter.h"

#include <ao/Error.h>
#include <ao/library/AudioIdentity.h>
#include <ao/library/FileManifestBuilder.h>
#include <ao/library/FileManifestLayout.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/TrackWriter.h>
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
    library::LibraryWrite& write,
    std::span<AudioIdentityWriteCandidate const> candidates)
  {
    auto writer = write.tracks();
    auto result = AudioIdentityBatchCommitResult{};

    for (auto const& candidate : candidates)
    {
      auto optCurrent = writer.manifest(candidate.uri);

      if (!optCurrent)
      {
        ++result.skippedCount;
        continue;
      }

      if (!matchesCandidate(*optCurrent, candidate))
      {
        ++result.skippedCount;
        continue;
      }

      auto builder = library::FileManifestBuilder::fromView(*optCurrent);
      builder.audioPayloadLength(candidate.identity.payloadLength).audioSignature(candidate.identity.signature);

      if (auto putRes = writer.updateManifest(optCurrent->trackId(), builder); !putRes)
      {
        return std::unexpected{putRes.error()};
      }

      ++result.completedCount;
    }

    return result;
  }
} // namespace ao::rt
