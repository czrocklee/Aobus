// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include "runtime/library/LibraryMutationService.h"
#include <ao/Error.h>
#include <ao/async/Task.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/WriteTransaction.h>

#include <expected>
#include <string>
#include <type_traits>
#include <utility>

namespace ao::rt::test
{
  template<typename Operation,
           typename OperationResult = std::remove_cvref_t<std::invoke_result_t<Operation, library::LibraryWrite&>>,
           typename Value = ::ao::rt::detail::OperationResultTraits<OperationResult>::ValueType>
  async::Task<Result<MutationExecution<Value>>> executeInteractiveMutation(
    LibraryMutationService::Submission submission,
    Operation operation,
    library::WriteTransaction::Options options = {},
    std::string operationName = "Library mutation")
  {
    auto mutationRes =
      co_await LibraryMutationService::beginInteractiveMutationAsync(std::move(submission), std::move(options));

    if (!mutationRes)
    {
      co_return std::unexpected{mutationRes.error()};
    }

    auto executionRes = co_await mutationRes->executeAsync(std::move(operation), std::move(operationName));
    co_return std::move(executionRes);
  }
} // namespace ao::rt::test
