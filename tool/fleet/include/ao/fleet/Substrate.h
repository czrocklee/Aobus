// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/fleet/Model.h>
#include <ao/fleet/ProcessRunner.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ao::fleet
{
  struct TreeFingerprint
  {
    std::string value;
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
    explicit SnapshotProvider(IProcessRunner& runner);

    Result<std::filesystem::path> createImmutableBase(std::filesystem::path const& repo,
                                                      std::filesystem::path const& destination);
    Result<std::filesystem::path> createWorkspace(std::filesystem::path const& base,
                                                  std::filesystem::path const& destination);
    void remove(std::filesystem::path const& path) noexcept;

  private:
    IProcessRunner& _runner;
  };

  class NamespaceRunner final
  {
  public:
    explicit NamespaceRunner(IProcessRunner& runner);

    ProcessResult run(std::filesystem::path const& realRepo,
                      std::filesystem::path const& workspace,
                      AuthorityPolicy const& authority,
                      ProcessRequest request);

  private:
    IProcessRunner& _runner;
  };

  class PatchExtractor final
  {
  public:
    explicit PatchExtractor(IProcessRunner& runner);
    Result<PatchArtifact> extract(std::filesystem::path const& workspace, std::string candidateId);
    Result<> apply(std::filesystem::path const& workspace, PatchArtifact const& patch);

  private:
    IProcessRunner& _runner;
  };

  struct PatchGuardResult
  {
    bool accepted = false;
    FailureReason failure = FailureReason::None;
    std::string detail;
  };

  class PatchGuard final
  {
  public:
    static PatchGuardResult inspect(PatchArtifact const& patch,
                                    std::vector<ScopeRule> const& scope,
                                    std::size_t churnLimit,
                                    std::vector<std::filesystem::path> const& rulerPaths);
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
} // namespace ao::fleet
