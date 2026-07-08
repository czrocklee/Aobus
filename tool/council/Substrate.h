// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "council/Model.h"
#include "council/ProcessRunner.h"
#include <ao/Error.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::council
{
  struct TreeFingerprint
  {
    std::string value = {};
    std::size_t entryCount = 0;

    bool operator==(TreeFingerprint const&) const = default;
  };

  class TreeCanary final
  {
  public:
    static Result<TreeFingerprint> fingerprint(std::filesystem::path const& root);
  };

  class SnapshotProvider final
  {
  public:
    explicit SnapshotProvider(ProcessRunner& runner);

    Result<std::filesystem::path> createImmutableBase(std::filesystem::path const& repo,
                                                      std::filesystem::path const& destination);
    Result<std::filesystem::path> createWorkspace(std::filesystem::path const& base,
                                                  std::filesystem::path const& destination);
    void remove(std::filesystem::path const& path) noexcept;

  private:
    ProcessRunner& _runner;
  };

  struct SandboxMounts
  {
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> writableBinds = {};
  };

  class NamespaceRunner final
  {
  public:
    explicit NamespaceRunner(ProcessRunner& runner);

    ProcessResult run(std::filesystem::path const& realRepo,
                      std::filesystem::path const& workspace,
                      SandboxMounts const& mounts,
                      ProcessRequest request);

  private:
    ProcessRunner& _runner;
  };

  class ArtifactStore final
  {
  public:
    explicit ArtifactStore(std::filesystem::path root);

    std::filesystem::path const& root() const noexcept;
    Result<> write(std::filesystem::path const& relativePath, std::string_view content) const;
    Result<> append(std::filesystem::path const& relativePath, std::string_view document) const;

  private:
    std::filesystem::path _root;
  };
} // namespace ao::council
