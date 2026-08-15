// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <set>
#include <string>
#include <string_view>

namespace ao::winui::test
{
  namespace
  {
    /// Every way the WinUI frontend can reach a declared string resource.
    struct ResourceReferences final
    {
      /// Ids handed straight to a lookup, which is where a bad spelling shows.
      std::set<std::string> ids;
      /**
       * @brief Every string the frontend spells at all.
       *
       * An id often reaches the loader through a local helper - a menu builder
       * naming its own items, say - so what proves a resource is still wanted
       * is that something still names it, not that the lookup is in view.
       */
      std::set<std::string> mentions;
      /// Prefixes a C++ lookup completes with runtime data, such as a field id.
      std::set<std::string> prefixes;
      /// `x:Uid` names, each of which claims every `<uid>.<property>` entry.
      std::set<std::string> uids;
    };

    std::string readFile(std::filesystem::path const& path)
    {
      auto stream = std::ifstream{path, std::ios::binary};
      REQUIRE(stream.is_open());
      return std::string{std::istreambuf_iterator{stream}, std::istreambuf_iterator<char>{}};
    }

    /// The literal that follows every occurrence of @p call in @p source.
    void collectCallLiterals(std::string_view const source, std::string_view const call, std::set<std::string>& into)
    {
      std::size_t position = 0;

      while ((position = source.find(call, position)) != std::string_view::npos)
      {
        position += call.size();
        auto rest = source.substr(position);

        // Both spellings the frontend uses: a narrow and a wide literal.
        if (rest.starts_with("L\""))
        {
          rest.remove_prefix(1);
        }

        if (!rest.starts_with('"'))
        {
          continue;
        }

        rest.remove_prefix(1);

        if (auto const end = rest.find('"'); end != std::string_view::npos)
        {
          into.emplace(rest.substr(0, end));
        }
      }
    }

    /// Every double-quoted literal in @p source, escapes included as written.
    void collectStringLiterals(std::string_view const source, std::set<std::string>& into)
    {
      auto position = source.find('"');

      while (position != std::string_view::npos)
      {
        auto end = source.find('"', position + 1);

        while (end != std::string_view::npos && source[end - 1] == '\\')
        {
          end = source.find('"', end + 1);
        }

        if (end == std::string_view::npos)
        {
          return;
        }

        into.emplace(source.substr(position + 1, end - position - 1));
        position = source.find('"', end + 1);
      }
    }

    /// The value each `<key>: <value>` line in @p source carries, one line at a time.
    void collectYamlScalars(std::string_view const source, std::string_view const key, std::set<std::string>& into)
    {
      auto const marker = std::string{key} + ":";
      std::size_t position = 0;

      while ((position = source.find(marker, position)) != std::string_view::npos)
      {
        position += marker.size();
        auto value = source.substr(position, source.find('\n', position) - position);

        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        {
          value.remove_prefix(1);
        }

        while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
        {
          value.remove_suffix(1);
        }

        if (!value.empty())
        {
          into.emplace(value);
        }
      }
    }

    /// Everything the shipped frontend sources, presets, and markup reach for.
    ResourceReferences frontendReferences()
    {
      constexpr auto kLookups =
        std::array<std::string_view, 4>{"resourceString(", "resourceHstring(", "resourceStringOr(", "formatResource("};
      auto references = ResourceReferences{};

      for (auto const& entry : std::filesystem::recursive_directory_iterator{AOBUS_WINDOWS_WINUI_DIR})
      {
        if (!entry.is_regular_file())
        {
          continue;
        }

        auto const extension = entry.path().extension().string();

        if (extension != ".cpp" && extension != ".h" && extension != ".xaml" && extension != ".yaml")
        {
          continue;
        }

        auto const source = readFile(entry.path());

        if (extension == ".cpp" || extension == ".h")
        {
          for (auto const lookup : kLookups)
          {
            collectCallLiterals(source, lookup, references.ids);
          }

          collectCallLiterals(source, "stableResourceString(", references.prefixes);
          collectStringLiterals(source, references.mentions);
          continue;
        }

        if (extension == ".yaml")
        {
          // A preset names a string through the shared `text` property, which
          // Windows resolves against the resource map before showing it.
          // Only `textResourceKey` names a resource. The shared `text` property
          // is the words themselves, so scanning it would report every literal
          // string in a preset as a missing resource id.
          collectYamlScalars(source, "textResourceKey", references.ids);
          collectYamlScalars(source, "textResourceKey", references.mentions);
          continue;
        }

        collectCallLiterals(source, "x:Uid=", references.uids);
      }

      return references;
    }

    std::set<std::string> declaredResourceNames()
    {
      auto const resw = readFile(std::filesystem::path{AOBUS_WINDOWS_RESW});
      auto names = std::set<std::string>{};
      constexpr auto kMarker = std::string_view{"<data name=\""};
      std::size_t position = 0;

      while ((position = resw.find(kMarker, position)) != std::string::npos)
      {
        position += kMarker.size();
        auto const end = resw.find('"', position);

        if (end == std::string::npos)
        {
          break;
        }

        names.emplace(resw.substr(position, end - position));
        position = end;
      }

      return names;
    }

    /// Whether anything in the frontend can produce @p name as a lookup.
    bool isReachable(ResourceReferences const& references, std::string const& name)
    {
      if (references.mentions.contains(name))
      {
        return true;
      }

      if (auto const dot = name.find('.'); dot != std::string::npos)
      {
        return references.uids.contains(name.substr(0, dot));
      }

      return std::ranges::any_of(references.prefixes, [&name](auto const& prefix) { return name.starts_with(prefix); });
    }
  } // namespace

  TEST_CASE("StringResource - the WinUI frontend resolves only resource ids the resource loader can find",
            "[winui][unit][layout]")
  {
    /*
     * A `.resw` name that carries a dot is the `x:Uid` convention: XAML splits
     * it into a target and a property, and the resource loader an ordinary C++
     * lookup goes through does not. Every string the shell used to reach that
     * way through `x:Uid` is now composed in C++, where the same name silently
     * resolves to nothing and the shell shows the id itself.
     */
    auto const references = frontendReferences();
    CHECK(references.ids.size() > 30);
    auto const declared = declaredResourceNames();

    for (auto const& id : references.ids)
    {
      INFO("resource id " << id);
      CHECK_FALSE(id.contains('.'));
      CHECK(declared.contains(id));
    }
  }

  TEST_CASE("StringResource - the WinUI frontend declares no string resource it cannot reach", "[winui][unit][layout]")
  {
    /*
     * A resource whose only reader was deleted keeps shipping in every language
     * the app is translated into, and reads as authority the next contributor
     * has to work out the fate of. Deleting the shell that used one is only
     * finished once its strings are gone with it, which is what this holds to.
     */
    auto const references = frontendReferences();

    for (auto const& name : declaredResourceNames())
    {
      INFO("declared resource " << name);
      CHECK(isReachable(references, name));
    }
  }
} // namespace ao::winui::test
