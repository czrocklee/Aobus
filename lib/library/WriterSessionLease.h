// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <filesystem>
#include <memory>

namespace ao::library::detail
{
  class WriterSessionLease final
  {
  public:
    static Result<WriterSessionLease> acquire(std::filesystem::path const& databasePath);

    ~WriterSessionLease();

    WriterSessionLease(WriterSessionLease const&) = delete;
    WriterSessionLease& operator=(WriterSessionLease const&) = delete;
    WriterSessionLease(WriterSessionLease&&) noexcept;
    WriterSessionLease& operator=(WriterSessionLease&&) noexcept;

  private:
    struct Impl;
    explicit WriterSessionLease(std::unique_ptr<Impl> implPtr);

    std::unique_ptr<Impl> _implPtr;
  };
} // namespace ao::library::detail
