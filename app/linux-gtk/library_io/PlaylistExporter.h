// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include <ao/library/MusicLibrary.h>

#include "track/TrackRowCache.h"
#include <runtime/TrackSource.h>

#include <sigc++/sigc++.h>

#include <filesystem>
#include <memory>

namespace ao::gtk
{
  class PlaylistExporter final : public rt::TrackSourceObserver
  {
  public:
    PlaylistExporter(rt::TrackSource& list,
                     TrackRowCache const& provider,
                     std::filesystem::path root,
                     std::filesystem::path path);
    ~PlaylistExporter() override;

    PlaylistExporter(PlaylistExporter const&) = delete;
    PlaylistExporter& operator=(PlaylistExporter const&) = delete;
    PlaylistExporter(PlaylistExporter&&) = delete;
    PlaylistExporter& operator=(PlaylistExporter&&) = delete;

    void triggerWrite();

    // TrackIdListObserver interface
    void onReset() override;
    void onInserted(TrackId id, std::size_t index) override;
    void onUpdated(TrackId id, std::size_t index) override;
    void onRemoved(TrackId id, std::size_t index) override;

  private:
    void writeFile();
    void scheduleForWrite();

    rt::TrackSource& _list;
    TrackRowCache const& _provider;
    std::filesystem::path const _root;
    std::filesystem::path const _path;
    std::unique_ptr<sigc::connection> _timeoutConnection;
  };
} // namespace ao::gtk