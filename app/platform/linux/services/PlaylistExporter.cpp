// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "platform/linux/services/PlaylistExporter.h"

#include "core/model/TrackIdList.h"
#include "core/model/TrackRowDataProvider.h"

#include <glibmm.h>

#include <fstream>
#include <iostream>

namespace app::services
{

PlaylistExporter::PlaylistExporter(app::core::model::TrackIdList& list,
                                   app::core::model::TrackRowDataProvider& provider,
                                   std::filesystem::path root,
                                   std::filesystem::path path)
  : _list{list}, _provider{provider}, _root{std::move(root)}, _path{std::move(path)}
{
  _list.attach(this);
  scheduleForWrite();
}

PlaylistExporter::~PlaylistExporter()
{
  _list.detach(this);
  if (_timeoutConnection)
  {
    _timeoutConnection->disconnect();
  }
}

void PlaylistExporter::onReset()
{
  scheduleForWrite();
}

void PlaylistExporter::onInserted(TrackId /*id*/, std::size_t /*index*/)
{
  scheduleForWrite();
}

void PlaylistExporter::onUpdated(TrackId /*id*/, std::size_t /*index*/)
{
  scheduleForWrite();
}

void PlaylistExporter::onRemoved(TrackId /*id*/, std::size_t /*index*/)
{
  scheduleForWrite();
}

void PlaylistExporter::scheduleForWrite()
{
  // Cancel any existing timeout
  if (_timeoutConnection)
  {
    _timeoutConnection->disconnect();
  }

  // Schedule write after 3 second delay (Glib::signal_timeout uses milliseconds)
  _timeoutConnection = std::make_unique<sigc::connection>(Glib::signal_timeout().connect(
    [this]()
    {
      writeFile();
      return false;
    },
    3000));
}

void PlaylistExporter::writeFile()
{
  _timeoutConnection->disconnect();
  _timeoutConnection.reset();

  auto ofs = std::ofstream{_path};

  if (!ofs)
  {
    std::cerr << "Failed to open playlist file: " << _path << std::endl;
    return;
  }

  // Export playlist from TrackId membership
  for (std::size_t i = 0; i < _list.size(); ++i)
  {
    auto id = _list.trackIdAt(i);

    // Try to get the URI path for this track
    auto optUri = _provider.getUriPath(id);
    if (optUri)
    {
      // Write path relative to playlist location
      auto relativePath = std::filesystem::relative(*optUri, _path.parent_path());
      ofs << relativePath.string() << std::endl;
    }
    else
    {
      // Fallback: write track ID as comment
      ofs << "# track://" << id.value() << " (URI not found)" << std::endl;
    }
  }
}

void PlaylistExporter::triggerWrite()
{
  scheduleForWrite();
}

} // namespace app::services
