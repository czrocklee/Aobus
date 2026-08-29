// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "TrackSourceLease.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace ao::library
{
  class MusicLibrary;
}

namespace ao::rt
{
  namespace detail
  {
    class RuntimeOperationProbe;
  }

  struct SourceSpec final
  {
    ListId baseListId = kInvalidListId;
    std::string filterExpression{};

    bool operator==(SourceSpec const&) const = default;
  };

  struct SourceSpecHash final
  {
    std::size_t operator()(SourceSpec const& spec) const noexcept;
  };

  class LibraryChanges;
  class TrackSourceCache final
  {
  public:
    TrackSourceCache(library::MusicLibrary const& library, LibraryChanges const& changes);
    ~TrackSourceCache();

    TrackSourceCache(TrackSourceCache const&) = delete;
    TrackSourceCache& operator=(TrackSourceCache const&) = delete;
    TrackSourceCache(TrackSourceCache&&) = delete;
    TrackSourceCache& operator=(TrackSourceCache&&) = delete;

    Result<TrackSourceLease> acquire(ListId listId);
    Result<TrackSourceLease> acquire(SourceSpec const& spec);
    std::optional<Error> sourceError(TrackSourceLease const& lease) const;
    void reloadAllTracks();

  private:
    struct Impl;
    std::unique_ptr<Impl> _implPtr;

    friend class detail::RuntimeOperationProbe;
  };
} // namespace ao::rt
