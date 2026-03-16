// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include "PlaylistExporter.h"
#include <fstream>

PlaylistExporter::PlaylistExporter(AbstractTrackList& list,
                                   std::filesystem::path root,
                                   std::filesystem::path path,
                                   QObject* parent)
  : QObject{parent}
  , _list{list}
  , _root{std::move(root)}
  , _path{std::move(path)}
  , _timer{new QTimer{this}}
{
  connect(_timer, &QTimer::timeout, this, &PlaylistExporter::writeFile);
  scheduleForWrite();
}

void PlaylistExporter::scheduleForWrite() { _timer->start(std::chrono::seconds{1}); }

void PlaylistExporter::writeFile()
{
  auto ofs = std::ofstream{_path};

  // Check if file opened successfully
  if (!ofs)
  {
    // std::cerr << "Failed to open file: " << filePath << '\n';
    return;
  }

  for (auto i = 0u; i < _list.size(); ++i)
  {
    auto const& [_, t] = _list.at(AbstractTrackList::Index{i});
    ofs << std::filesystem::relative(_root / t.prop->filepath, _path.parent_path()).c_str() << std::endl;
  }
}