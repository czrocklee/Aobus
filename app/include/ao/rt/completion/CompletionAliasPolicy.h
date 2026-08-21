// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  inline constexpr std::size_t kMinimumCompletionAliasLength = 3;

  /** Derives transient completion-only spellings from admitted source text. */
  class CompletionAliasPolicy
  {
  public:
    CompletionAliasPolicy() = default;
    CompletionAliasPolicy(CompletionAliasPolicy const&) = delete;
    CompletionAliasPolicy(CompletionAliasPolicy&&) = delete;
    CompletionAliasPolicy& operator=(CompletionAliasPolicy const&) = delete;
    CompletionAliasPolicy& operator=(CompletionAliasPolicy&&) = delete;
    virtual ~CompletionAliasPolicy() = default;

    /** Replaces caller-owned storage with lower-case ASCII alias keys. */
    virtual Result<> makeAliasesInto(std::vector<std::string>& output, std::string_view admittedUtf8Text) const = 0;
  };
} // namespace ao::rt
