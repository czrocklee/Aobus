/*
 * Copyright (C) <year> <name of author>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of  MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

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
    const auto& [_, t] = _list.at(AbstractTrackList::Index{i});
    ofs << std::filesystem::relative(_root / t.prop->filepath, _path.parent_path()).c_str() << std::endl;
  }
}