// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "app/GtkAccelTranslator.h"

#include <ao/uimodel/input/KeyChord.h>

#include <gdk/gdk.h>
#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <gtkmm/accelerator.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    using uimodel::KeyChord;
    using uimodel::KeyModifier;
    using uimodel::KeyModifiers;

    // Neutral tokens whose spelling differs from gdk_keyval_name(), plus media keys.
    // Everything else (Right, Home, F5, letters, digits) round-trips through GDK directly.
    constexpr auto kAliases = std::array<std::pair<std::string_view, guint>, 10>{{
      {"Space", GDK_KEY_space},
      {"Enter", GDK_KEY_Return},
      {"Backspace", GDK_KEY_BackSpace},
      {"PageUp", GDK_KEY_Page_Up},
      {"PageDown", GDK_KEY_Page_Down},
      {"Media:Play", GDK_KEY_AudioPlay},
      {"Media:Pause", GDK_KEY_AudioPause},
      {"Media:Stop", GDK_KEY_AudioStop},
      {"Media:Next", GDK_KEY_AudioNext},
      {"Media:Prev", GDK_KEY_AudioPrev},
    }};

    std::optional<guint> aliasKeyval(std::string_view token)
    {
      for (auto const& [name, keyval] : kAliases)
      {
        if (name == token)
        {
          return keyval;
        }
      }

      return std::nullopt;
    }

    std::optional<std::string> aliasToken(guint keyval)
    {
      for (auto const& [name, kv] : kAliases)
      {
        if (kv == keyval)
        {
          return std::string{name};
        }
      }

      return std::nullopt;
    }

    Gdk::ModifierType toGdkModifiers(KeyModifiers modifiers)
    {
      auto mods = Gdk::ModifierType{};

      if (modifiers.has(KeyModifier::Ctrl))
      {
        mods |= Gdk::ModifierType::CONTROL_MASK;
      }

      if (modifiers.has(KeyModifier::Shift))
      {
        mods |= Gdk::ModifierType::SHIFT_MASK;
      }

      if (modifiers.has(KeyModifier::Alt))
      {
        mods |= Gdk::ModifierType::ALT_MASK;
      }

      if (modifiers.has(KeyModifier::Super))
      {
        mods |= Gdk::ModifierType::SUPER_MASK;
      }

      return mods;
    }

    KeyModifiers fromGdkModifiers(Gdk::ModifierType mods)
    {
      auto const hasMask = [mods](Gdk::ModifierType mask) { return static_cast<bool>(mods & mask); };
      auto result = KeyModifiers{};

      if (hasMask(Gdk::ModifierType::CONTROL_MASK))
      {
        result |= KeyModifier::Ctrl;
      }

      if (hasMask(Gdk::ModifierType::SHIFT_MASK))
      {
        result |= KeyModifier::Shift;
      }

      if (hasMask(Gdk::ModifierType::ALT_MASK))
      {
        result |= KeyModifier::Alt;
      }

      if (hasMask(Gdk::ModifierType::SUPER_MASK))
      {
        result |= KeyModifier::Super;
      }

      return result;
    }

    guint tokenToKeyval(std::string const& token)
    {
      if (auto const optAlias = aliasKeyval(token); optAlias)
      {
        return *optAlias;
      }

      if (token.size() == 1)
      {
        auto const lowered = static_cast<gunichar>(std::tolower(static_cast<unsigned char>(token.front())));

        if (auto const keyval = ::gdk_unicode_to_keyval(lowered); keyval != 0)
        {
          return keyval;
        }
      }

      if (auto const keyval = ::gdk_keyval_from_name(token.c_str()); keyval != 0 && keyval != GDK_KEY_VoidSymbol)
      {
        return keyval;
      }

      return 0;
    }

    // Printable ASCII range used to distinguish single-character key names from tokens like
    // named keys (Right, Home, etc.) that gdk_keyval_name() handles natively.
    constexpr auto kMinPrintableAscii = std::int32_t{0x21};
    constexpr auto kMaxPrintableAscii = std::int32_t{0x7E};

    // Keyvals that only modify other keys; a chord is incomplete while one of these is the
    // pressed key, so interactive capture must ignore them and keep waiting for a real key.
    bool isStandaloneModifier(guint keyval)
    {
      switch (keyval)
      {
        case GDK_KEY_Control_L:
        case GDK_KEY_Control_R:
        case GDK_KEY_Shift_L:
        case GDK_KEY_Shift_R:
        case GDK_KEY_Shift_Lock:
        case GDK_KEY_Caps_Lock:
        case GDK_KEY_Alt_L:
        case GDK_KEY_Alt_R:
        case GDK_KEY_Meta_L:
        case GDK_KEY_Meta_R:
        case GDK_KEY_Super_L:
        case GDK_KEY_Super_R:
        case GDK_KEY_Hyper_L:
        case GDK_KEY_Hyper_R:
        case GDK_KEY_ISO_Level3_Shift:
        case GDK_KEY_Num_Lock: return true;
        default: return false;
      }
    }

    std::optional<std::string> keyvalToToken(guint keyval)
    {
      if (auto const optAlias = aliasToken(keyval); optAlias)
      {
        return optAlias;
      }

      if (auto const codepoint = ::gdk_keyval_to_unicode(keyval);
          codepoint >= kMinPrintableAscii && codepoint <= kMaxPrintableAscii)
      {
        auto ch = static_cast<char>(codepoint);

        if (std::isalpha(static_cast<unsigned char>(ch)) != 0)
        {
          ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }

        return std::string{ch};
      }

      if (auto const* name = ::gdk_keyval_name(keyval); name != nullptr)
      {
        return std::string{name};
      }

      return std::nullopt;
    }
  } // namespace

  std::optional<std::string> toGtkAccel(KeyChord const& chord)
  {
    if (!chord.valid())
    {
      return std::nullopt;
    }

    auto const keyval = tokenToKeyval(chord.key);

    if (keyval == 0)
    {
      return std::nullopt;
    }

    return Gtk::Accelerator::name(keyval, toGdkModifiers(chord.modifiers)).raw();
  }

  std::optional<KeyChord> fromGtkAccel(std::string const& accel)
  {
    guint keyval = guint{0};
    auto mods = Gdk::ModifierType{};

    if (!Gtk::Accelerator::parse(accel, keyval, mods) || keyval == 0)
    {
      return std::nullopt;
    }

    auto optToken = keyvalToToken(keyval);

    if (!optToken)
    {
      return std::nullopt;
    }

    return KeyChord{.modifiers = fromGdkModifiers(mods), .key = std::move(*optToken)};
  }

  std::optional<KeyChord> fromGtkKeyval(guint keyval, Gdk::ModifierType state)
  {
    if (keyval == 0 || isStandaloneModifier(keyval))
    {
      return std::nullopt;
    }

    auto optToken = keyvalToToken(keyval);

    if (!optToken)
    {
      return std::nullopt;
    }

    return KeyChord{.modifiers = fromGdkModifiers(state), .key = std::move(*optToken)};
  }
} // namespace ao::gtk
