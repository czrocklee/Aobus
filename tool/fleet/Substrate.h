// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "fleet/Model.h"
#include "fleet/ProcessRunner.h"
#include <ao/Error.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
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

  struct SandboxMounts
  {
    // Host path -> sandbox path pairs mounted writable inside the namespace, applied after the
    // workspace bind so they can nest under the virtualized repository path or under /tmp.
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> writableBinds;
    // Bind $HOME read-write. The namespace is a path virtualizer, not a security boundary;
    // model CLIs need their normal credential and config files to authenticate.
    bool bindHome = false;
  };

  class NamespaceRunner final
  {
  public:
    explicit NamespaceRunner(IProcessRunner& runner);

    ProcessResult run(std::filesystem::path const& realRepo,
                      std::filesystem::path const& workspace,
                      SandboxMounts const& mounts,
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
