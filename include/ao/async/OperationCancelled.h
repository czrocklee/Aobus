// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <exception>
#include <stop_token>

namespace ao::async
{
  class OperationCancelled final : public std::exception
  {
  public:
    char const* what() const noexcept override { return "operation cancelled"; }
  };

  bool isOperationCancelled(std::exception const& exception);
  [[noreturn]] void throwOperationCancelled();
  void throwIfStopRequested(std::stop_token stopToken);
  void rethrowIfOperationCancelled(std::exception const& exception);
  void rethrowIfOperationCancelled();
} // namespace ao::async
