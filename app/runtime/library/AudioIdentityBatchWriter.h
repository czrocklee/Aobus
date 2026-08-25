// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "AudioIdentityIndexer.h"
#include <ao/Error.h>

#include <span>

namespace ao::library
{
  class LibraryWrite;
}

namespace ao::rt
{
  Result<AudioIdentityBatchCommitResult> applyAudioIdentityBatch(
    library::LibraryWrite& write,
    std::span<AudioIdentityWriteCandidate const> candidates);
} // namespace ao::rt
