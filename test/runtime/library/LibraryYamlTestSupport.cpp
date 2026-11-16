// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/runtime/library/LibraryYamlTestSupport.h"

#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <unordered_set>

namespace ao::rt::test
{
  namespace
  {
    // Only a direct field of a tracks sequence entry has nullable mtime
    // semantics; custom metadata may use both "mtime" and "uri" as text keys.
    bool isTrackMtime(ryml::ConstNodeRef const& node)
    {
      auto const record = node.parent();

      if (!node.has_key() || ao::yaml::keyView(node) != "mtime" || !record.readable() || !record.is_map() ||
          record.has_key())
      {
        return false;
      }

      auto const tracks = record.parent();
      return tracks.readable() && tracks.is_seq() && tracks.has_key() && ao::yaml::keyView(tracks) == "tracks";
    }

    // The fixture equality of two parsed nodes. REQUIRE aborts the case on a
    // structural mismatch, so later code can rely on both nodes agreeing in
    // kind; `path` names the node in every failure through INFO.
    void checkNodeEqual(ryml::ConstNodeRef const& expected, ryml::ConstNodeRef const& actual, std::string const& path)
    {
      INFO("at " << path);
      REQUIRE(expected.is_map() == actual.is_map());
      REQUIRE(expected.is_seq() == actual.is_seq());
      REQUIRE(expected.has_val() == actual.has_val());

      if (expected.is_map())
      {
        // Equal counts and unique expected keys make the found/unfound check
        // complete: an actual duplicate or extra key leaves one expected key
        // missing. The importer rejects duplicate keys, so one is a fixture
        // difference, not a last-writer-wins collapse.
        REQUIRE(expected.num_children() == actual.num_children());
        auto seenKeys = std::unordered_set<std::string_view>{};

        for (auto const& expectedChild : expected.children())
        {
          auto const key = ao::yaml::keyView(expectedChild);
          REQUIRE(seenKeys.insert(key).second);
          auto const actualChild = ao::yaml::findChild(actual, key);
          REQUIRE(actualChild.readable());
          checkNodeEqual(expectedChild, actualChild, path + "." + std::string{key});
        }
      }
      else if (expected.is_seq())
      {
        // Sequence order is content, not layout.
        REQUIRE(expected.num_children() == actual.num_children());

        for (std::size_t index = 0; index < expected.num_children(); ++index)
        {
          checkNodeEqual(expected.child(index), actual.child(index), std::format("{}[{}]", path, index));
        }
      }
      else if (expected.has_val())
      {
        // Raw scalar lexemes: implicit typing never makes two spellings
        // equal, and quoting only delimits text, so a plain, quoted, or
        // block spelling of one text is one value.
        CHECK(ao::yaml::scalarView(expected) == ao::yaml::scalarView(actual));

        // A track record's mtime, the schema's one nullable field, also
        // compares its explicit-null form: an unquoted null is not the
        // quoted text "null".
        if (isTrackMtime(expected))
        {
          CHECK(expected.val_is_null() == actual.val_is_null());
        }
      }
    }

    // Parses and resolves a document the way the importer does. FAIL is
    // fatal, so an unusable document ends the case here.
    void parseDocument(ryml::Tree& tree, std::string_view document, std::string_view label)
    {
      auto state = ao::yaml::ErrorCallbackState{std::string{label}};

      if (auto const parsedRes = ao::yaml::parseInArena(tree, document, state); !parsedRes)
      {
        FAIL(label << ": " << parsedRes.error().message);
      }

      if (auto const resolvedRes = ao::yaml::resolve(tree, state); !resolvedRes)
      {
        FAIL(label << ": " << resolvedRes.error().message);
      }
    }
  } // namespace

  void checkLibraryYamlFixtureEqual(std::string_view expectedDocument, std::string_view actualDocument)
  {
    auto expectedTree = ryml::Tree{ao::yaml::callbacks()};
    auto actualTree = ryml::Tree{ao::yaml::callbacks()};

    parseDocument(expectedTree, expectedDocument, "expected document");
    parseDocument(actualTree, actualDocument, "actual document");

    checkNodeEqual(expectedTree.rootref(), actualTree.rootref(), "document");
  }
} // namespace ao::rt::test
