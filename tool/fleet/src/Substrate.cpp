// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/fleet/Model.h>
#include <ao/fleet/ProcessRunner.h>
#include <ao/fleet/Serialization.h>
#include <ao/fleet/Substrate.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::fleet
{
  namespace
  {
    constexpr auto kFnvOffsetBasis = std::uint64_t{1469598103934665603ULL};
    constexpr auto kDefaultLocalTimeout = std::chrono::minutes{20};

    bool ignoredCanaryPath(std::filesystem::path const& relative)
    {
      if (relative.empty())
      {
        return false;
      }

      auto const first = *relative.begin();
      return first == ".cache" || first == "build" || first == "logs";
    }

    void hashBytes(std::uint64_t& hash, std::string_view bytes)
    {
      constexpr auto kPrime = std::uint64_t{1099511628211ULL};

      for (auto const byte : bytes)
      {
        hash ^= static_cast<unsigned char>(byte);
        hash *= kPrime;
      }
    }

    ProcessRequest localRequest(std::vector<std::string> argv, std::filesystem::path const& cwd)
    {
      return ProcessRequest{
        .argv = std::move(argv),
        .cwd = cwd,
        .standardInput = {},
        .environmentWhitelist = {"PATH", "HOME", "USER", "TMPDIR", "NIX_PATH", "GIT_CONFIG_NOSYSTEM"},
        .environment = {},
        .timeout = kDefaultLocalTimeout,
        .terminationGrace = std::chrono::seconds{2},
      };
    }

    Result<> requireSuccess(ProcessResult const& result, std::string_view operation)
    {
      if (result.status != ProcessStatus::Exited || result.exitCode != 0)
      {
        return makeError(Error::Code::IoError, std::format("{} failed: {}", operation, result.standardError));
      }

      return {};
    }

    std::filesystem::path normalizeDiffPath(std::string_view path)
    {
      auto result = std::string{path};

      if (result.starts_with("a/") || result.starts_with("b/"))
      {
        result.erase(0, 2);
      }

      return std::filesystem::path{result}.lexically_normal();
    }

    bool pathCovered(std::filesystem::path const& candidate, std::filesystem::path const& ruler)
    {
      auto const normalizedCandidate = candidate.lexically_normal().generic_string();
      auto normalizedRuler = ruler.lexically_normal().generic_string();

      if (!normalizedRuler.ends_with('/'))
      {
        normalizedRuler += '/';
      }

      return normalizedCandidate == ruler.lexically_normal().generic_string() ||
             normalizedCandidate.starts_with(normalizedRuler);
    }
  } // namespace

  Result<TreeFingerprint> TreeCanary::fingerprint(std::filesystem::path const& root)
  {
    try
    {
      auto entries = std::vector<std::filesystem::path>{};
      auto options = std::filesystem::directory_options::skip_permission_denied;

      for (auto iterator = std::filesystem::recursive_directory_iterator{root, options};
           iterator != std::filesystem::recursive_directory_iterator{};
           ++iterator)
      {
        auto const relative = std::filesystem::relative(iterator->path(), root);

        if (ignoredCanaryPath(relative))
        {
          if (iterator->is_directory())
          {
            iterator.disable_recursion_pending();
          }

          continue;
        }

        entries.emplace_back(relative);
      }

      std::ranges::sort(entries, {}, [](auto const& path) { return path.generic_string(); });
      auto hash = std::uint64_t{kFnvOffsetBasis};

      for (auto const& relative : entries)
      {
        auto const absolute = root / relative;
        auto const status = std::filesystem::symlink_status(absolute);
        hashBytes(hash, relative.generic_string());
        hashBytes(hash, std::format(":{}:", static_cast<std::uint32_t>(status.permissions())));

        if (std::filesystem::is_symlink(status))
        {
          hashBytes(hash, std::filesystem::read_symlink(absolute).generic_string());
        }
        else if (std::filesystem::is_regular_file(status))
        {
          auto input = std::ifstream{absolute, std::ios::binary};
          auto buffer = std::array<char, 8192>{};

          while (input)
          {
            input.read(buffer.data(), buffer.size());
            hashBytes(hash, std::string_view{buffer.data(), static_cast<std::size_t>(input.gcount())});
          }
        }
      }

      auto output = std::ostringstream{};
      output << std::hex << std::setfill('0') << std::setw(16) << hash;
      return TreeFingerprint{.value = output.str(), .entryCount = entries.size()};
    }
    catch (std::exception const& exception)
    {
      return makeError(Error::Code::IoError, exception.what());
    }
  }

  SnapshotProvider::SnapshotProvider(IProcessRunner& runner)
    : _runner{runner}
  {
  }

  Result<std::filesystem::path> SnapshotProvider::createImmutableBase(std::filesystem::path const& repo,
                                                                      std::filesystem::path const& destination)
  {
    auto error = std::error_code{};
    std::filesystem::create_directories(destination.parent_path(), error);

    if (error)
    {
      return makeError(Error::Code::IoError, error.message());
    }

    auto btrfsCheck = _runner.run(localRequest({"btrfs", "subvolume", "show", repo.string()}, repo));
    auto btrfsSnapshot = false;

    if (btrfsCheck.status == ProcessStatus::Exited && btrfsCheck.exitCode == 0)
    {
      auto snapshot =
        _runner.run(localRequest({"btrfs", "subvolume", "snapshot", repo.string(), destination.string()}, repo));
      btrfsSnapshot = snapshot.status == ProcessStatus::Exited && snapshot.exitCode == 0;
    }

    if (!btrfsSnapshot)
    {
      auto sync = _runner.run(localRequest({"rsync",
                                            "-a",
                                            "--delete",
                                            "--exclude=.cache",
                                            "--exclude=build",
                                            "--exclude=logs",
                                            repo.string() + "/",
                                            destination.string() + "/"},
                                           repo));

      if (auto success = requireSuccess(sync, "snapshot copy"); !success)
      {
        return std::unexpected{success.error()};
      }
    }

    auto add = _runner.run(localRequest({"git", "-C", destination.string(), "add", "-A"}, destination));

    if (auto success = requireSuccess(add, "snapshot index"); !success)
    {
      return std::unexpected{success.error()};
    }

    auto commitRequest = localRequest({"git",
                                       "-C",
                                       destination.string(),
                                       "-c",
                                       "core.hooksPath=/dev/null",
                                       "-c",
                                       "user.name=Aobus Fleet",
                                       "-c",
                                       "user.email=fleet@localhost",
                                       "commit",
                                       "--allow-empty",
                                       "--no-gpg-sign",
                                       "-m",
                                       "fleet immutable baseline"},
                                      destination);
    auto commit = _runner.run(commitRequest);

    if (auto success = requireSuccess(commit, "snapshot commit"); !success)
    {
      return std::unexpected{success.error()};
    }

    if (btrfsSnapshot)
    {
      auto readOnly = _runner.run(localRequest(
        {"btrfs", "property", "set", "-ts", destination.string(), "ro", "true"}, destination.parent_path()));

      if (auto success = requireSuccess(readOnly, "immutable snapshot seal"); !success)
      {
        return std::unexpected{success.error()};
      }
    }

    return destination;
  }

  Result<std::filesystem::path> SnapshotProvider::createWorkspace(std::filesystem::path const& base,
                                                                  std::filesystem::path const& destination)
  {
    auto error = std::error_code{};
    std::filesystem::create_directories(destination.parent_path(), error);

    if (error)
    {
      return makeError(Error::Code::IoError, error.message());
    }

    auto sync =
      _runner.run(localRequest({"rsync", "-a", "--delete", base.string() + "/", destination.string() + "/"}, base));

    if (auto success = requireSuccess(sync, "workspace copy"); !success)
    {
      return std::unexpected{success.error()};
    }

    return destination;
  }

  void SnapshotProvider::remove(std::filesystem::path const& path) noexcept
  {
    try
    {
      if (!std::filesystem::exists(path))
      {
        return;
      }

      auto check = _runner.run(localRequest({"btrfs", "subvolume", "show", path.string()}, path.parent_path()));

      if (check.status == ProcessStatus::Exited && check.exitCode == 0)
      {
        auto removed = _runner.run(localRequest({"btrfs", "subvolume", "delete", path.string()}, path.parent_path()));

        if (removed.status == ProcessStatus::Exited && removed.exitCode == 0)
        {
          return;
        }
      }

      auto error = std::error_code{};
      std::filesystem::remove_all(path, error);
    }
    catch (...)
    {
      // Best-effort cleanup ignores filesystem and subprocess failures.
      return;
    }
  }

  NamespaceRunner::NamespaceRunner(IProcessRunner& runner)
    : _runner{runner}
  {
  }

  ProcessResult NamespaceRunner::run(std::filesystem::path const& realRepo,
                                     std::filesystem::path const& workspace,
                                     AuthorityPolicy const& authority,
                                     ProcessRequest request)
  {
    auto argv = std::vector<std::string>{"bwrap",
                                         "--die-with-parent",
                                         "--new-session",
                                         "--ro-bind",
                                         "/",
                                         "/",
                                         "--dev-bind",
                                         "/dev",
                                         "/dev",
                                         "--proc",
                                         "/proc",
                                         "--tmpfs",
                                         "/tmp"};

    if (authority.network == NetworkAuthority::Off)
    {
      argv.emplace_back("--unshare-net");
    }

    argv.emplace_back(authority.filesystem == FilesystemAuthority::ReadOnly ? "--ro-bind" : "--bind");
    argv.emplace_back(workspace.string());
    argv.emplace_back(realRepo.string());
    argv.emplace_back("--chdir");
    argv.emplace_back(realRepo.string());
    argv.insert(argv.end(), request.argv.begin(), request.argv.end());
    request.argv = std::move(argv);
    request.cwd = workspace;
    return _runner.run(request);
  }

  PatchExtractor::PatchExtractor(IProcessRunner& runner)
    : _runner{runner}
  {
  }

  Result<PatchArtifact> PatchExtractor::extract(std::filesystem::path const& workspace, std::string candidateId)
  {
    auto intentToAdd = _runner.run(localRequest({"git", "-C", workspace.string(), "add", "-N", "--", "."}, workspace));

    if (auto success = requireSuccess(intentToAdd, "untracked-file discovery"); !success)
    {
      return std::unexpected{success.error()};
    }

    auto patchResult = _runner.run(
      localRequest({"git", "-C", workspace.string(), "diff", "--binary", "--no-ext-diff", "HEAD", "--"}, workspace));

    if (auto success = requireSuccess(patchResult, "patch extraction"); !success)
    {
      return std::unexpected{success.error()};
    }

    auto statusResult =
      _runner.run(localRequest({"git", "-C", workspace.string(), "diff", "--name-status", "HEAD", "--"}, workspace));

    if (auto success = requireSuccess(statusResult, "patch status extraction"); !success)
    {
      return std::unexpected{success.error()};
    }

    auto statResult =
      _runner.run(localRequest({"git", "-C", workspace.string(), "diff", "--numstat", "HEAD", "--"}, workspace));

    if (auto success = requireSuccess(statResult, "patch statistics extraction"); !success)
    {
      return std::unexpected{success.error()};
    }

    auto result = PatchArtifact{.candidateId = std::move(candidateId),
                                .patch = std::move(patchResult.standardOutput),
                                .touchedFiles = {},
                                .addedLines = 0,
                                .removedLines = 0};
    auto statusStream = std::istringstream{statusResult.standardOutput};
    auto row = std::string{};

    while (std::getline(statusStream, row))
    {
      auto const tab = row.find('\t');

      if (tab == std::string::npos)
      {
        continue;
      }

      auto path = row.substr(tab + 1);

      if (auto renameTab = path.rfind('\t'); renameTab != std::string::npos)
      {
        path = path.substr(renameTab + 1);
      }

      result.touchedFiles.emplace_back(normalizeDiffPath(path));
    }

    auto statStream = std::istringstream{statResult.standardOutput};

    while (std::getline(statStream, row))
    {
      auto first = row.find('\t');
      auto second = first == std::string::npos ? first : row.find('\t', first + 1);

      if (first == std::string::npos || second == std::string::npos)
      {
        continue;
      }

      auto parseCount = [](std::string_view text)
      {
        auto value = std::size_t{};
        auto conversion = std::from_chars(text.data(), text.data() + text.size(), value);
        return conversion.ec == std::errc{} ? value : std::size_t{};
      };
      result.addedLines += parseCount(std::string_view{row}.substr(0, first));
      result.removedLines += parseCount(std::string_view{row}.substr(first + 1, second - first - 1));
    }

    return result;
  }

  Result<> PatchExtractor::apply(std::filesystem::path const& workspace, PatchArtifact const& patch)
  {
    auto request =
      localRequest({"git", "-C", workspace.string(), "apply", "--binary", "--whitespace=nowarn", "-"}, workspace);
    request.standardInput = patch.patch;
    auto applied = _runner.run(request);

    if (auto success = requireSuccess(applied, "patch apply"); !success)
    {
      return std::unexpected{success.error()};
    }

    return {};
  }

  PatchGuardResult PatchGuard::inspect(PatchArtifact const& patch,
                                       std::vector<ScopeRule> const& scope,
                                       std::size_t churnLimit,
                                       std::vector<std::filesystem::path> const& rulerPaths)
  {
    if (patch.patch.empty() || patch.touchedFiles.empty())
    {
      return {.failure = FailureReason::NoCandidate, .detail = "candidate produced no patch"};
    }

    if (patch.addedLines + patch.removedLines > churnLimit)
    {
      return {.failure = FailureReason::ChurnExceeded, .detail = "candidate exceeds churn limit"};
    }

    if (patch.patch.find("old mode ") != std::string::npos || patch.patch.find("new mode ") != std::string::npos ||
        patch.patch.find("new file mode 120000") != std::string::npos ||
        patch.patch.find("deleted file mode 120000") != std::string::npos ||
        patch.patch.find("rename from ") != std::string::npos || patch.patch.find("copy from ") != std::string::npos)
    {
      return {.failure = FailureReason::ScopeViolation, .detail = "mode and symlink changes are forbidden"};
    }

    for (auto const& path : patch.touchedFiles)
    {
      if (std::ranges::any_of(rulerPaths, [&](auto const& ruler) { return pathCovered(path, ruler); }))
      {
        return {
          .failure = FailureReason::ScopeViolation, .detail = std::format("ruler path changed: {}", path.string())};
      }

      auto const rule = std::ranges::find_if(scope, [&](ScopeRule const& candidate) { return candidate.path == path; });

      if (rule == scope.end())
      {
        return {
          .failure = FailureReason::ScopeViolation, .detail = std::format("out-of-scope path: {}", path.string())};
      }

      auto operation = ScopeOperation::Modify;
      auto const newMarker = std::format("diff --git a/{0} b/{0}\nnew file mode", path.generic_string());
      auto const deleteMarker = std::format("diff --git a/{0} b/{0}\ndeleted file mode", path.generic_string());

      if (patch.patch.find(newMarker) != std::string::npos)
      {
        operation = ScopeOperation::Create;
      }

      if (patch.patch.find(deleteMarker) != std::string::npos)
      {
        operation = ScopeOperation::Delete;
      }

      if (!rule->operations.contains(operation))
      {
        return {.failure = FailureReason::ScopeViolation,
                .detail = std::format("operation '{}' not allowed for {}", toString(operation), path.string())};
      }
    }

    return {.accepted = true, .failure = FailureReason::None, .detail = "accepted"};
  }

  ArtifactStore::ArtifactStore(std::filesystem::path root)
    : _root{std::move(root)}
  {
  }

  std::filesystem::path const& ArtifactStore::root() const noexcept
  {
    return _root;
  }

  Result<> ArtifactStore::write(std::filesystem::path const& relativePath, std::string_view content) const
  {
    try
    {
      auto const path = _root / relativePath;
      std::filesystem::create_directories(path.parent_path());
      auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
      output.write(content.data(), static_cast<std::streamsize>(content.size()));
      output.close();

      if (!output)
      {
        return makeError(Error::Code::IoError, std::format("cannot write {}", path.string()));
      }

      return {};
    }
    catch (std::exception const& exception)
    {
      return makeError(Error::Code::IoError, exception.what());
    }
  }

  Result<> ArtifactStore::append(std::filesystem::path const& relativePath, std::string_view document) const
  {
    auto appended = appendYamlDocument(_root / relativePath, document);

    if (!appended)
    {
      return makeError(Error::Code::IoError, appended.error().message);
    }

    return {};
  }
} // namespace ao::fleet
