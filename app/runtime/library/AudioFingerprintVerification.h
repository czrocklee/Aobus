// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "LibraryWriteLane.h"
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/compat/MoveOnlyFunction.h>
#include <ao/library/AudioIdentity.h>
#include <ao/rt/library/AudioFingerprintReport.h>

#include <stop_token>
#include <string>
#include <string_view>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  Result<AudioFingerprintReport> inspectAudioFingerprint(library::MusicLibrary const& source,
                                                         std::string_view uri,
                                                         std::stop_token stopToken,
                                                         library::AudioIdentityProgressCallback progress = {});

  using AudioFingerprintInspection = compat::MoveOnlyFunction<Result<AudioFingerprintReport>(std::stop_token)>;

  // Run the bound file inspection in the active lane turn, with both caller and Closing
  // cancellation. Only after it succeeds may the writer recheck and adopt the observation.
  async::Task<Result<AudioFingerprintReport>> confirmAudioFingerprintAsync(
    LibraryWriteLane::Submission submission,
    LibraryWriteLane::BackgroundTaskLease const* backgroundTask,
    library::MusicLibrary const* source,
    std::string confirmation,
    std::stop_token stopToken,
    AudioFingerprintInspection inspect);
} // namespace ao::rt
