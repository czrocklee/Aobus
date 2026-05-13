// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "flac/File.h"
#include "mp4/File.h"
#include "mpeg/File.h"
#include <ao/tag/TagFile.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace ao::tag
{
  // static
  std::unique_ptr<TagFile> TagFile::open(std::filesystem::path const& path, TagFile::Mode mode)
  {
    using Creator = std::unique_ptr<TagFile> (*)(std::filesystem::path const&, TagFile::Mode);

    static constexpr auto CreatorMap = std::array{
      std::pair{std::string_view{".mp3"},
                +[](std::filesystem::path const& filePath, TagFile::Mode fileMode) -> std::unique_ptr<TagFile>
                { return std::make_unique<mpeg::File>(filePath, fileMode); }},
      std::pair{std::string_view{".m4a"},
                +[](std::filesystem::path const& filePath, TagFile::Mode fileMode) -> std::unique_ptr<TagFile>
                { return std::make_unique<mp4::File>(filePath, fileMode); }},
      std::pair{std::string_view{".flac"},
                +[](std::filesystem::path const& filePath, TagFile::Mode fileMode) -> std::unique_ptr<TagFile>
                { return std::make_unique<flac::File>(filePath, fileMode); }},
    };

    auto ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char ch) { return std::tolower(ch); });

    if (auto const* const it =
          std::ranges::find(CreatorMap, std::string_view{ext}, &std::pair<std::string_view, Creator>::first);
        it != CreatorMap.end())
    {
      return it->second(path, mode);
    }

    return nullptr;
  }
} // namespace ao::tag
