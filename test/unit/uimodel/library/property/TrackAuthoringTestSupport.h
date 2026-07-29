// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/CoreIds.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ao::rt
{
  class Library;
  class LibraryChanges;
} // namespace ao::rt

namespace ao::uimodel::test
{
  class TrackAuthoringFixture final
  {
  public:
    explicit TrackAuthoringFixture(std::size_t trackCount = 2);
    ~TrackAuthoringFixture();

    TrackAuthoringFixture(TrackAuthoringFixture const&) = delete;
    TrackAuthoringFixture& operator=(TrackAuthoringFixture const&) = delete;
    TrackAuthoringFixture(TrackAuthoringFixture&&) = delete;
    TrackAuthoringFixture& operator=(TrackAuthoringFixture&&) = delete;

    rt::Library& library() const;
    rt::LibraryChanges& changes() const;
    std::span<TrackId const> trackIds() const noexcept;

    std::string title(TrackId trackId) const;
    std::vector<std::string> tags(TrackId trackId) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::uimodel::test
