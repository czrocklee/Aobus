// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "test/TestFixtureSupport.h"

#include <filesystem>

namespace ao::test
{
  // TempDir for a probe child that may terminate with an LMDB environment open.
  // Darwin LMDB locks each environment through two named POSIX semaphores that
  // outlive an aborted owner. Only a later exclusive open of the same lock file
  // unlinks them, and removing the directory leaves nothing to open. Destruction
  // therefore opens and closes each environment below the directory first.
  //
  // Destroy it only after the child is reaped and every other owner of an
  // environment below it, including one in this process, is closed.
  class [[nodiscard]] LmdbProbeScratch final
  {
  public:
    LmdbProbeScratch() = default;
    ~LmdbProbeScratch() noexcept;

    LmdbProbeScratch(LmdbProbeScratch const&) = delete;
    LmdbProbeScratch& operator=(LmdbProbeScratch const&) = delete;
    LmdbProbeScratch(LmdbProbeScratch&&) = delete;
    LmdbProbeScratch& operator=(LmdbProbeScratch&&) = delete;

    std::filesystem::path const& path() const;

  private:
    TempDir _directory;
  };
} // namespace ao::test
