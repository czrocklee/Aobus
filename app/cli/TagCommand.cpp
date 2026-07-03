// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TagCommand.h"

#include "CliContext.h"
#include "CommandError.h"
#include "Output.h"
#include "TrackSelection.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/library/LibraryWriter.h>

#include <CLI/App.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::cli
{
  namespace
  {
    void printJsonStringArray(std::ostream& os, std::vector<std::string> const& values)
    {
      auto array = JsonArray{os};

      for (auto const& value : values)
      {
        array.element();
        std::print(os, "{}", jsonQuote(value));
      }
    }

    void printJsonTrackIdArray(std::ostream& os, std::span<TrackId const> ids)
    {
      auto array = JsonArray{os};

      for (auto const id : ids)
      {
        array.element();
        std::print(os, "{}", id.raw());
      }
    }

    std::vector<TrackId> resolveTargets(CliContext& context,
                                        std::vector<std::uint32_t> const& rawIds,
                                        std::string const& filter)
    {
      if (!rawIds.empty() && !filter.empty())
      {
        throwCommandError(Error::Code::InvalidInput, "tag command accepts either explicit ids or --filter, not both");
      }

      if (rawIds.empty() && filter.empty())
      {
        throwCommandError(Error::Code::InvalidInput, "tag command requires track ids or --filter");
      }

      if (!filter.empty())
      {
        return queryMatchingTrackIds(context.musicLibrary(), filter);
      }

      auto reader = context.library().reader();
      return requireTrackIds(reader, rawIds);
    }

    void printTags(std::span<TrackId const> trackIds,
                   std::vector<std::string> const& tagNames,
                   OutputFormat format,
                   std::ostream& os)
    {
      auto const singleTrack = trackIds.size() == 1;

      if (format == OutputFormat::Yaml)
      {
        if (singleTrack)
        {
          std::println(os, "trackId: {}", trackIds[0]);
        }
        else
        {
          std::println(os, "trackIds:");

          for (auto const trackId : trackIds)
          {
            std::println(os, "  - {}", trackId.raw());
          }
        }

        if (tagNames.empty())
        {
          std::println(os, "tags: []");
          return;
        }

        std::println(os, "tags:");

        for (auto const& name : tagNames)
        {
          std::println(os, "  - {}", yamlQuote(name));
        }

        return;
      }

      if (format == OutputFormat::Json)
      {
        auto object = JsonObject{os};

        if (singleTrack)
        {
          object.uintField("trackId", trackIds[0].raw());
        }
        else
        {
          object.field("trackIds");
          printJsonTrackIdArray(os, trackIds);
        }

        object.field("tags");
        printJsonStringArray(os, tagNames);
        object.close();
        std::println(os);
        return;
      }

      if (tagNames.empty())
      {
        std::println(os, "no tags");
        return;
      }

      std::print(os, "tags: ");

      for (auto const& name : tagNames)
      {
        std::print(os, "{} ", name);
      }

      std::println(os);
    }

    void showTags(CliContext& context, std::vector<std::uint32_t> const& rawIds)
    {
      auto reader = context.library().reader();
      auto const trackIds = requireTrackIds(reader, rawIds);
      auto const tags = reader.selectionTags(trackIds);
      printTags(trackIds, tags, context.options().format, context.io().out);
    }

    void formatMutation(std::string_view action,
                        std::string const& tagName,
                        rt::EditTrackTagsReply const& reply,
                        OutputFormat format,
                        std::ostream& os)
    {
      if (format == OutputFormat::Yaml)
      {
        yamlKeyValue(os, 0, "action", action);
        yamlKeyValue(os, 0, "tag", tagName);
        yamlKeyValue(os, 0, "updated", static_cast<std::uint64_t>(reply.mutatedIds.size()));

        if (reply.mutatedIds.empty())
        {
          std::println(os, "trackIds: []");
          return;
        }

        std::println(os, "trackIds:");

        for (auto const trackId : reply.mutatedIds)
        {
          std::println(os, "  - {}", trackId.raw());
        }

        return;
      }

      if (format == OutputFormat::Json)
      {
        auto object = JsonObject{os};
        object.stringField("action", action);
        object.stringField("tag", tagName);
        object.uintField("updated", static_cast<std::uint64_t>(reply.mutatedIds.size()));
        object.field("trackIds");
        printJsonTrackIdArray(os, reply.mutatedIds);
        object.close();
        std::println(os);
        return;
      }

      std::println(os,
                   "{} tag: {} {} {} track(s)",
                   action == "add" ? "added" : "removed",
                   tagName,
                   action == "add" ? "to" : "from",
                   reply.mutatedIds.size());
    }

    void editTags(CliContext& context,
                  bool add,
                  std::string const& tagName,
                  std::vector<std::uint32_t> const& rawIds,
                  std::string const& filter)
    {
      auto const trackIds = resolveTargets(context, rawIds, filter);
      auto const tags = std::array{tagName};
      auto const replyResult = add
                                 ? context.library().writer().editTags(trackIds, tags, std::span<std::string const>{})
                                 : context.library().writer().editTags(trackIds, std::span<std::string const>{}, tags);

      if (!replyResult)
      {
        throwCommandError(replyResult.error());
      }

      formatMutation(add ? "add" : "remove", tagName, *replyResult, context.options().format, context.io().out);
    }

    void listTags(CliContext& context)
    {
      auto const tags = context.library().reader().allTagsByFrequency();

      if (context.options().format == OutputFormat::Yaml)
      {
        if (tags.empty())
        {
          std::println(context.io().out, "tags: []");
          return;
        }

        std::println(context.io().out, "tags:");

        for (auto const& [name, count] : tags)
        {
          std::println(context.io().out, "  - name: {}", yamlQuote(name));
          yamlKeyValue(context.io().out, 4, "count", static_cast<std::uint64_t>(count));
        }

        return;
      }

      if (context.options().format == OutputFormat::Json)
      {
        for (auto const& [name, count] : tags)
        {
          auto object = JsonObject{context.io().out};
          object.stringField("name", name);
          object.uintField("count", static_cast<std::uint64_t>(count));
          object.close();
          std::println(context.io().out);
        }

        return;
      }

      for (auto const& [name, count] : tags)
      {
        std::println(context.io().out, "{}  {}", name, count);
      }
    }
  } // namespace

  void setupTagCommand(CLI::App& app, CliContext& context)
  {
    auto* tag = app.add_subcommand("tag", "Tag management commands");
    tag->require_subcommand(1);

    tag->add_subcommand("list", "List tags by frequency")->callback([&context] { listTags(context); });

    auto* add = tag->add_subcommand("add", "Add a tag to tracks");
    auto* addTagName = add->add_option("tag", "tag name")->required();
    auto addIdsPtr = std::make_shared<std::vector<std::uint32_t>>();
    add->add_option("id", *addIdsPtr, "track id");
    auto* addFilter = add->add_option("-f,--filter", "track filter expression");
    add->callback(
      [&context, addTagName, addIdsPtr, addFilter]
      {
        auto const tagName = addTagName->as<std::string>();
        auto const filter = addFilter->count() > 0 ? addFilter->as<std::string>() : std::string{};
        editTags(context, true, tagName, *addIdsPtr, filter);
      });

    auto* remove = tag->add_subcommand("remove", "Remove a tag from tracks");
    auto* remTagName = remove->add_option("tag", "tag name")->required();
    auto remIdsPtr = std::make_shared<std::vector<std::uint32_t>>();
    remove->add_option("id", *remIdsPtr, "track id");
    auto* remFilter = remove->add_option("-f,--filter", "track filter expression");
    remove->callback(
      [&context, remTagName, remIdsPtr, remFilter]
      {
        auto const tagName = remTagName->as<std::string>();
        auto const filter = remFilter->count() > 0 ? remFilter->as<std::string>() : std::string{};
        editTags(context, false, tagName, *remIdsPtr, filter);
      });

    auto* show = tag->add_subcommand("show", "Show tags shared by selected tracks");
    auto showIdsPtr = std::make_shared<std::vector<std::uint32_t>>();
    show->add_option("id", *showIdsPtr, "track id")->required();
    show->callback([&context, showIdsPtr] { showTags(context, *showIdsPtr); });
  }
} // namespace ao::cli
