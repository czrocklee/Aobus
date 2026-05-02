// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <ao/library/MusicLibrary.h>

#include "platform/linux/ui/TrackRowDataProvider.h"
#include <ao/model/TrackIdList.h>

#include <sigc++/sigc++.h>

#include <filesystem>
#include <memory>

namespace app::services
{
  class PlaylistExporter final : public ao::model::TrackIdListObserver
  {
  public:
    using TrackId = ao::TrackId;

    PlaylistExporter(ao::model::TrackIdList& list,
                     app::ui::TrackRowDataProvider const& provider,
                     std::filesystem::path root,
                     std::filesystem::path path);
    ~PlaylistExporter() override;

    void triggerWrite();

    // TrackIdListObserver interface
    void onReset() override;
    void onInserted(TrackId id, std::size_t index) override;
    void onUpdated(TrackId id, std::size_t index) override;
    void onRemoved(TrackId id, std::size_t index) override;

  private:
    void writeFile();
    void scheduleForWrite();

    ao::model::TrackIdList& _list;
    app::ui::TrackRowDataProvider const& _provider;
    std::filesystem::path const _root;
    std::filesystem::path const _path;
    std::unique_ptr<sigc::connection> _timeoutConnection;
  };
} // namespace app::services