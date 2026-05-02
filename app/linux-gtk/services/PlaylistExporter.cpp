// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "PlaylistExporter.h"
#include <ao/utility/Log.h>

#include "TrackRowDataProvider.h"
#include <ao/model/TrackIdList.h>

#include <glibmm.h>

#include <fstream>
#include <iostream>

namespace ao::gtk::services
{
  PlaylistExporter::PlaylistExporter(ao::model::TrackIdList& list,
                                     ao::gtk::TrackRowDataProvider const& provider,
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
      3000)); // NOLINT(readability-magic-numbers)
  }

  void PlaylistExporter::writeFile()
  {
    _timeoutConnection->disconnect();
    _timeoutConnection.reset();

    auto ofs = std::ofstream{_path};

    if (!ofs)
    {
      APP_LOG_ERROR("Failed to open playlist file: {}", _path.string());
      return;
    }

    // Export playlist from TrackId membership
    for (std::size_t i = 0; i < _list.size(); ++i)
    {
      auto id = _list.trackIdAt(i);

      if (auto optUri = _provider.getUriPath(id); optUri)
      {
        // Write path relative to playlist location
        auto relativePath = std::filesystem::relative(*optUri, _path.parent_path());
        ofs << relativePath.string() << '\n';
      }
      else
      {
        // Fallback: write track ID as comment
        ofs << "# track://" << id.value() << " (URI not found)" << '\n';
      }
    }
  }

  void PlaylistExporter::triggerWrite()
  {
    scheduleForWrite();
  }
} // namespace ao::gtk::services
