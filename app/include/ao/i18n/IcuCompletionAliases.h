// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/rt/completion/CompletionAliasPolicy.h>

#include <memory>

namespace ao::i18n
{
  /** Creates the owner-thread-confined ICU completion alias policy. */
  std::unique_ptr<rt::CompletionAliasPolicy> createIcuCompletionAliasPolicy();
} // namespace ao::i18n
