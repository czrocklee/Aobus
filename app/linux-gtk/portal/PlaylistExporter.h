// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#pragma once

#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/rt/TrackSource.h>

#include <sigc++/connection.h>

#include <cstddef>
#include <filesystem>
#include <memory>

namespace ao::gtk::portal
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
} // namespace ao::gtk::portal
