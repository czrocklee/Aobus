// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <rs/core/MusicLibrary.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class ImportWorker
{
public:
  using ProgressCallback = std::function<void(std::filesystem::path const& path, std::int32_t itemIndex)>;
  using FinishedCallback = std::function<void()>;

  ImportWorker(rs::core::MusicLibrary& ml,
               std::vector<std::filesystem::path> const& files,
               ProgressCallback progressCallback,
               FinishedCallback finishedCallback);

  void run();
  void commit();

private:
  rs::core::MusicLibrary& _ml;
  std::optional<rs::lmdb::WriteTransaction> _txn;
  std::vector<std::filesystem::path> _files;
  std::string _rootPathStr;
  ProgressCallback _progressCallback;
  FinishedCallback _finishedCallback;
};
