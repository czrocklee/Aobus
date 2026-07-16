// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/library/AudioIdentityIndexer.h>

#include <span>

namespace ao::library
{
  class MusicLibrary;
  class WriteTransaction;
}

namespace ao::rt
{
  Result<AudioIdentityBatchCommitResult> applyAudioIdentityBatch(
    library::MusicLibrary& library,
    library::WriteTransaction& transaction,
    std::span<AudioIdentityWriteCandidate const> candidates);
} // namespace ao::rt
