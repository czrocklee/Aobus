// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/yaml/Reflect.h>

#include <cstdint>
#include <ostream>
#include <string>

namespace ao::cli
{
  enum class OutputFormat : std::uint8_t
  {
    Plain,
    Yaml,
    Json,
  };

  namespace detail
  {
    void emitGeneratedDocument(std::ostream& os, std::string const& text);
  } // namespace detail

  // Boost.PFR member-name reflection requires DTO types with external linkage
  // on MSVC; do not pass function-local or anonymous-namespace aggregates.
  template<typename T>
  void emitDocument(std::ostream& os, OutputFormat format, T const& dto)
  {
    if (format == OutputFormat::Yaml)
    {
      detail::emitGeneratedDocument(os, yaml::toYamlString(dto));
    }
    else if (format == OutputFormat::Json)
    {
      detail::emitGeneratedDocument(os, yaml::toJsonString(dto));
    }
  }
} // namespace ao::cli
