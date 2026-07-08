// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "TagCommand.h"

#include "CliContext.h"
#include "CommandError.h"
#include "DryRunFlag.h"
#include "Output.h"
#include "TrackSelection.h"
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/rt/CoreRuntime.h>
#include <ao/rt/TrackMutation.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/yaml/Reflect.h>

#include <CLI/App.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::cli
{
  struct TagSelectionDto final
  {
    std::optional<TrackId> optTrackId{};
    std::optional<std::vector<TrackId>> optTrackIds{};
    std::vector<std::string> tags{};
  };
} // namespace ao::cli

template<>
struct ao::yaml::ReflectNameOverrides<ao::cli::TagSelectionDto>
{
  static constexpr std::string_view keyFor(std::string_view memberName) noexcept
  {
    if (memberName == "optTrackId")
    {
      return "trackId";
    }

    if (memberName == "optTrackIds")
    {
      return "trackIds";
    }

    return memberName;
  }
};

namespace ao::cli
{
  namespace
  {
    struct TagFrequencyDto final
    {
      std::string name{};
      std::uint64_t count = 0;
    };

    struct TagFrequencyListDto final
    {
      std::vector<TagFrequencyDto> tags{};
    };

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
      if (format != OutputFormat::Plain)
      {
        bool const singleTrack = trackIds.size() == 1;

        emitDocument(os,
                     format,
                     TagSelectionDto{.optTrackId = singleTrack ? std::optional{trackIds[0]} : std::nullopt,
                                     .optTrackIds = singleTrack ? std::nullopt
                                                                : std::optional{std::vector<TrackId>{
                                                                    trackIds.begin(), trackIds.end()}},
                                     .tags = tagNames});
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

    void printSelectedTags(CliContext& context, std::vector<std::uint32_t> const& rawIds)
    {
      auto reader = context.library().reader();
      auto const trackIds = requireTrackIds(reader, rawIds);
      auto const tags = reader.selectionTags(trackIds);
      printTags(trackIds, tags, context.options().format, context.io().out);
    }

    struct TagMutationReportDto final
    {
      std::string action{};
      std::string tag{};
      bool dryRun = false;
      std::uint64_t updated = 0;
      std::vector<TrackId> trackIds{};
      std::vector<rt::TrackTagsChange> changes{};
    };

    void formatMutation(std::string_view action,
                        std::string const& tagName,
                        rt::EditTrackTagsReply const& reply,
                        bool dryRun,
                        OutputFormat format,
                        std::ostream& os)
    {
      if (format != OutputFormat::Plain)
      {
        emitDocument(os,
                     format,
                     TagMutationReportDto{.action = std::string{action},
                                          .tag = tagName,
                                          .dryRun = dryRun,
                                          .updated = static_cast<std::uint64_t>(reply.mutatedIds.size()),
                                          .trackIds = reply.mutatedIds,
                                          .changes = reply.changes});
        return;
      }

      std::println(os,
                   "{} tag: {} {} {} track(s){}",
                   action == "add" ? "added" : "removed",
                   tagName,
                   action == "add" ? "to" : "from",
                   reply.mutatedIds.size(),
                   dryRun ? " (dry-run)" : "");
    }

    void editTags(CliContext& context,
                  bool add,
                  std::string const& tagName,
                  std::vector<std::uint32_t> const& rawIds,
                  std::string const& filter,
                  bool dryRun)
    {
      auto const trackIds = resolveTargets(context, rawIds, filter);
      auto const tags = std::array{tagName};

      if (dryRun)
      {
        auto const replyResult =
          add ? context.library().writer().previewEditTags(trackIds, tags, std::span<std::string const>{})
              : context.library().writer().previewEditTags(trackIds, std::span<std::string const>{}, tags);

        if (!replyResult)
        {
          throwCommandError(replyResult.error());
        }

        formatMutation(add ? "add" : "remove", tagName, *replyResult, true, context.options().format, context.io().out);
        return;
      }

      auto const replyResult = add
                                 ? context.library().writer().editTags(trackIds, tags, std::span<std::string const>{})
                                 : context.library().writer().editTags(trackIds, std::span<std::string const>{}, tags);

      if (!replyResult)
      {
        throwCommandError(replyResult.error());
      }

      formatMutation(add ? "add" : "remove", tagName, *replyResult, false, context.options().format, context.io().out);
    }

    void listTags(CliContext& context)
    {
      auto const tags = context.library().reader().allTagsByFrequency();

      if (context.options().format != OutputFormat::Plain)
      {
        auto report = TagFrequencyListDto{};
        report.tags.reserve(tags.size());

        for (auto const& [name, count] : tags)
        {
          report.tags.push_back(TagFrequencyDto{.name = name, .count = static_cast<std::uint64_t>(count)});
        }

        emitDocument(context.io().out, context.options().format, report);
        return;
      }

      for (auto const& [name, count] : tags)
      {
        std::println(context.io().out, "{}  {}", name, count);
      }
    }
  } // namespace

  void configureTagCommand(CLI::App& app, CliContext& context)
  {
    auto* tag = app.add_subcommand("tag", "Tag management commands");
    tag->require_subcommand(1);

    tag->add_subcommand("list", "List tags by frequency")->callback([&context] { listTags(context); });

    auto* add = tag->add_subcommand("add", "Add a tag to tracks");
    auto* addTagName = add->add_option("tag", "tag name")->required();
    auto addIdsPtr = std::make_shared<std::vector<std::uint32_t>>();
    add->add_option("id", *addIdsPtr, "track id");
    auto* addFilter = add->add_option("-f,--filter", "track filter expression");
    auto* addDryRun = addDryRunFlag(*add);
    add->callback(
      [&context, addTagName, addIdsPtr, addFilter, addDryRun]
      {
        auto const tagName = addTagName->as<std::string>();
        auto const filter = addFilter->count() > 0 ? addFilter->as<std::string>() : std::string{};
        editTags(context, true, tagName, *addIdsPtr, filter, isDryRun(addDryRun));
      });

    auto* remove = tag->add_subcommand("remove", "Remove a tag from tracks");
    auto* remTagName = remove->add_option("tag", "tag name")->required();
    auto remIdsPtr = std::make_shared<std::vector<std::uint32_t>>();
    remove->add_option("id", *remIdsPtr, "track id");
    auto* remFilter = remove->add_option("-f,--filter", "track filter expression");
    auto* remDryRun = addDryRunFlag(*remove);
    remove->callback(
      [&context, remTagName, remIdsPtr, remFilter, remDryRun]
      {
        auto const tagName = remTagName->as<std::string>();
        auto const filter = remFilter->count() > 0 ? remFilter->as<std::string>() : std::string{};
        editTags(context, false, tagName, *remIdsPtr, filter, isDryRun(remDryRun));
      });

    auto* show = tag->add_subcommand("show", "Show tags shared by selected tracks");
    auto showIdsPtr = std::make_shared<std::vector<std::uint32_t>>();
    show->add_option("id", *showIdsPtr, "track id")->required();
    show->callback([&context, showIdsPtr] { printSelectedTags(context, *showIdsPtr); });
  }
} // namespace ao::cli
