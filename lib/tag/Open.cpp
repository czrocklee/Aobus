// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/tag/File.h>
#include <rs/tag/flac/File.h>
#include <rs/tag/mp4/File.h>
#include <rs/tag/mpeg/File.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace rs::tag
{
  // static
  std::unique_ptr<File> File::open(std::filesystem::path const& path, File::Mode mode)
  {
    using Creator = std::unique_ptr<File> (*)(std::filesystem::path const&, File::Mode);

    static constexpr auto CreatorMap = std::array{
      std::pair{std::string_view{".mp3"},
                +[](std::filesystem::path const& filePath, File::Mode fileMode) -> std::unique_ptr<File>
                { return std::make_unique<mpeg::File>(filePath, fileMode); }},
      std::pair{std::string_view{".m4a"},
                +[](std::filesystem::path const& filePath, File::Mode fileMode) -> std::unique_ptr<File>
                { return std::make_unique<mp4::File>(filePath, fileMode); }},
      std::pair{std::string_view{".flac"},
                +[](std::filesystem::path const& filePath, File::Mode fileMode) -> std::unique_ptr<File>
                { return std::make_unique<flac::File>(filePath, fileMode); }},
    };

    auto ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char ch) { return std::tolower(ch); });

    if (auto const it =
          std::ranges::find(CreatorMap, std::string_view{ext}, &std::pair<std::string_view, Creator>::first);
        it != CreatorMap.end())
    {
      return it->second(path, mode);
    }

    return nullptr;
  }
} // namespace rs::tag
