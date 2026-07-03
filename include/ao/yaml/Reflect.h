// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/utility/EnumName.h>
#include <ao/utility/StrongType.h>
#include <ao/yaml/Utils.h>

#include <boost/pfr.hpp>

#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace ao::yaml
{
  template<typename T>
  struct ReflectScalarTraits
  {};

  // Specialize for an enum by providing a static `names()` that returns a
  // utility::EnumNameTable mapping each enumerator to its serialized string.
  template<typename T>
  struct ReflectEnumTraits
  {};

  template<typename T>
  struct ReflectNameOverrides
  {
    static constexpr std::string_view keyFor(std::string_view memberName) noexcept { return memberName; }
  };

  namespace detail
  {
    template<typename T>
    using Bare = std::remove_cvref_t<T>;

    // std::to_chars scratch buffers, sized for the widest supported representation:
    // 20 digits + sign for 64-bit integers, and the shortest round-trip form of a double.
    inline constexpr std::size_t kIntegralCharsBufferSize = 32;
    inline constexpr std::size_t kFloatCharsBufferSize = 128;

    template<typename T>
    struct IsOptional : std::false_type
    {};

    template<typename T>
    struct IsOptional<std::optional<T>> : std::true_type
    {};

    template<typename T>
    struct IsVector : std::false_type
    {};

    template<typename T, typename Alloc>
    struct IsVector<std::vector<T, Alloc>> : std::true_type
    {};

    template<typename T>
    struct IsStringMap : std::false_type
    {};

    template<typename T, typename Compare, typename Alloc>
    struct IsStringMap<std::map<std::string, T, Compare, Alloc>> : std::true_type
    {};

    template<typename T>
    concept ReflectOptional = IsOptional<Bare<T>>::value;

    template<typename T>
    concept ReflectSequence = IsVector<Bare<T>>::value;

    template<typename T>
    concept ReflectMap = IsStringMap<Bare<T>>::value;

    template<typename T>
    concept ReflectScalar =
      requires(ryml::NodeRef node, T const& value) { ReflectScalarTraits<Bare<T>>::write(node, value); };

    template<typename T>
    concept ReflectEnum =
      std::is_enum_v<Bare<T>> && requires(T value) { utility::enumName(ReflectEnumTraits<Bare<T>>::names(), value); };

    template<typename T>
    concept ReflectStruct = std::is_aggregate_v<Bare<T>> && (!ReflectScalar<T>) && (!ReflectEnum<T>) &&
                            (!ReflectOptional<T>) && (!ReflectSequence<T>) && (!ReflectMap<T>);

    inline void writeQuotedString(ryml::NodeRef node, std::string_view value)
    {
      node |= ryml::VAL;
      node |= ryml::VAL_DQUO;
      setValue(node, value);
    }

    inline void writePlainScalar(ryml::NodeRef node, std::string_view value)
    {
      node |= ryml::VAL;
      node |= ryml::VAL_PLAIN;
      setValue(node, value);
    }

    template<typename T>
    void writeValue(ryml::NodeRef node, T const& value);

    template<ReflectScalar T>
    void writeScalarValue(ryml::NodeRef node, T const& value)
    {
      ReflectScalarTraits<Bare<T>>::write(node, value);
    }

    template<ReflectEnum T>
    void writeEnumValue(ryml::NodeRef node, T value)
    {
      writeQuotedString(node, utility::enumName(ReflectEnumTraits<Bare<T>>::names(), value));
    }

    template<ReflectOptional T>
    void writeOptionalValue(ryml::NodeRef node, T const& value)
    {
      if (value)
      {
        writeValue(node, *value);
      }
      else
      {
        writeQuotedString(node, {});
      }
    }

    template<ReflectSequence T>
    void writeSequenceValue(ryml::NodeRef node, T const& value)
    {
      node |= ryml::SEQ;

      for (auto const& item : value)
      {
        writeValue(node.append_child(), item);
      }
    }

    template<ReflectMap T>
    void writeMapValue(ryml::NodeRef node, T const& value)
    {
      node |= ryml::MAP;

      for (auto const& [key, item] : value)
      {
        auto child = node.append_child();
        setKey(child, key);
        writeValue(child, item);
      }
    }

    template<ReflectStruct T>
    void writeStructValue(ryml::NodeRef node, T const& value)
    {
      node |= ryml::MAP;

      auto const names = boost::pfr::names_as_array<Bare<T>>();
      std::size_t fieldIndex = 0;

      boost::pfr::for_each_field(value,
                                 [&](auto const& field)
                                 {
                                   auto const memberName = std::string_view{names[fieldIndex++]};

                                   if constexpr (ReflectOptional<decltype(field)>)
                                   {
                                     if (!field)
                                     {
                                       return;
                                     }
                                   }

                                   auto child = node.append_child();
                                   setKey(child, ReflectNameOverrides<Bare<T>>::keyFor(memberName));
                                   writeValue(child, field);
                                 });
    }

    template<typename T>
    void writeValue(ryml::NodeRef node, T const& value)
    {
      if constexpr (ReflectScalar<T>)
      {
        writeScalarValue(node, value);
      }
      else if constexpr (ReflectEnum<T>)
      {
        writeEnumValue(node, value);
      }
      else if constexpr (ReflectOptional<T>)
      {
        writeOptionalValue(node, value);
      }
      else if constexpr (ReflectSequence<T>)
      {
        writeSequenceValue(node, value);
      }
      else if constexpr (ReflectMap<T>)
      {
        writeMapValue(node, value);
      }
      else
      {
        static_assert(ReflectStruct<T>, "Type is not supported by ao::yaml reflection");
        writeStructValue(node, value);
      }
    }
  } // namespace detail

  template<>
  struct ReflectScalarTraits<std::string>
  {
    static void write(ryml::NodeRef node, std::string const& value) { detail::writeQuotedString(node, value); }
  };

  template<>
  struct ReflectScalarTraits<std::string_view>
  {
    static void write(ryml::NodeRef node, std::string_view value) { detail::writeQuotedString(node, value); }
  };

  template<>
  struct ReflectScalarTraits<char const*>
  {
    static void write(ryml::NodeRef node, char const* value)
    {
      detail::writeQuotedString(node, value != nullptr ? std::string_view{value} : std::string_view{});
    }
  };

  template<>
  struct ReflectScalarTraits<bool>
  {
    static void write(ryml::NodeRef node, bool value) { detail::writePlainScalar(node, value ? "true" : "false"); }
  };

  template<std::integral T>
    requires(!std::same_as<T, bool>)
  struct ReflectScalarTraits<T>
  {
    static void write(ryml::NodeRef node, T value)
    {
      auto buffer = std::array<char, detail::kIntegralCharsBufferSize>{};
      auto const [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
      std::ignore = ec;
      detail::writePlainScalar(node, std::string_view{buffer.data(), static_cast<std::size_t>(ptr - buffer.data())});
    }
  };

  template<std::floating_point T>
  struct ReflectScalarTraits<T>
  {
    static void write(ryml::NodeRef node, T value)
    {
      auto buffer = std::array<char, detail::kFloatCharsBufferSize>{};
      auto const [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
      std::ignore = ec;
      detail::writePlainScalar(node, std::string_view{buffer.data(), static_cast<std::size_t>(ptr - buffer.data())});
    }
  };

  template<>
  struct ReflectScalarTraits<std::filesystem::path>
  {
    static void write(ryml::NodeRef node, std::filesystem::path const& value)
    {
      detail::writeQuotedString(node, value.string());
    }
  };

  template<typename T, typename Tag>
  struct ReflectScalarTraits<utility::StrongType<T, Tag>>
  {
    static void write(ryml::NodeRef node, utility::StrongType<T, Tag> const& value)
    {
      detail::writeValue(node, value.raw());
    }
  };

  template<typename T>
  ryml::Tree buildTree(T const& dto)
  {
    auto tree = ryml::Tree{callbacks()};
    detail::writeValue(tree.rootref(), dto);
    return tree;
  }

  template<typename T>
  std::string toYamlString(T const& dto)
  {
    auto const tree = buildTree(dto);
    return ryml::emitrs_yaml<std::string>(tree);
  }

  template<typename T>
  std::string toJsonString(T const& dto)
  {
    auto const tree = buildTree(dto);
    return ryml::emitrs_json<std::string>(tree);
  }
} // namespace ao::yaml
