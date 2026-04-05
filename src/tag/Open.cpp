// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#include <rs/tag/File.h>
#include <rs/tag/flac/File.h>
#include <rs/tag/mp4/File.h>
#include <rs/tag/mpeg/File.h>

#include <algorithm>
#include <functional>
#include <unordered_map>

namespace rs::tag
{
  // static
  std::unique_ptr<File> File::open(std::filesystem::path const& path, File::Mode mode)
  {
    static std::unordered_map<std::string, std::function<std::unique_ptr<File>(std::filesystem::path const, File::Mode)>> const
      CreatorMap = {
        {".mp3", [](auto const& p, File::Mode m) { return std::make_unique<mpeg::File>(p, m); }},
        {".m4a", [](auto const& p, File::Mode m) { return std::make_unique<mp4::File>(p, m); }},
        {".flac", [](auto const& p, File::Mode m) { return std::make_unique<flac::File>(p, m); }}};

    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (auto it = CreatorMap.find(ext); it != CreatorMap.end()) { return it->second(path, mode); }
    return nullptr;
  }
} // namespace rs::tag
