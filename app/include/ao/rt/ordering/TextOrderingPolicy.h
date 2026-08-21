// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <string>
#include <string_view>

namespace ao::rt
{
  /** Derives transient binary keys for one explicitly selected text order. */
  class TextOrderingPolicy
  {
  public:
    TextOrderingPolicy() = default;
    TextOrderingPolicy(TextOrderingPolicy const&) = delete;
    TextOrderingPolicy(TextOrderingPolicy&&) = delete;
    TextOrderingPolicy& operator=(TextOrderingPolicy const&) = delete;
    TextOrderingPolicy& operator=(TextOrderingPolicy&&) = delete;
    virtual ~TextOrderingPolicy() = default;

    /**
     * Replaces caller-owned storage with a length-aware binary key, not UTF-8 text.
     *
     * The empty source retains the empty/missing ordering bucket. Returned
     * non-empty keys contain no library, serialization, or locale ownership.
     * The caller may reuse output capacity across successive values.
     */
    virtual Result<> makeSortKeyInto(std::string& output, std::string_view admittedUtf8Text) const = 0;
  };
} // namespace ao::rt
