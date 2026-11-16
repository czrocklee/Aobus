// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <string_view>

namespace ao::rt::test
{
  /**
   * @brief Checks that a library YAML export holds the content of its frozen
   *        fixture document.
   *
   * Fixture equality is the parsed structure this format's tests pin:
   * - node kinds and map key sets, but never mapping key order;
   * - sequence order and length;
   * - each scalar's exact text, so no spelling collapses ("007" is not "7",
   *   "2.50" is not "2.5") and quoting only delimits text: a plain, quoted,
   *   or block spelling of one text is one value, as in a recording-date's
   *   equivalent clear forms;
   * - a track record's mtime, the schema's one nullable field, also compares
   *   its explicit-null form, so an unquoted null is not the quoted text
   *   "null";
   * - a missing key and a key with an empty value are different, and a
   *   duplicate map key is a difference, because the importer rejects any
   *   document that carries one.
   *
   * Quoting style, indentation, and flow versus block layout are the
   * exporter's choice and do not take part.
   */
  void checkLibraryYamlFixtureEqual(std::string_view expectedDocument, std::string_view actualDocument);
} // namespace ao::rt::test
