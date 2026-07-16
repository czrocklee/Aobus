// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "FilesystemTestSupport.h"

#include <aclapi.h>
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::test
{
  namespace
  {
    struct [[nodiscard]] Handle final
    {
      HANDLE value = nullptr;

      Handle() = default;

      ~Handle() noexcept
      {
        if (value != nullptr)
        {
          ::CloseHandle(value);
        }
      }

      Handle(Handle const&) = delete;
      Handle& operator=(Handle const&) = delete;
      Handle(Handle&&) = delete;
      Handle& operator=(Handle&&) = delete;
    };

    class LocalSecurityDescriptor final
    {
    public:
      explicit LocalSecurityDescriptor(PSECURITY_DESCRIPTOR value)
        : _value{value}
      {
      }

      ~LocalSecurityDescriptor() noexcept
      {
        if (_value != nullptr)
        {
          ::LocalFree(_value);
        }
      }

      LocalSecurityDescriptor(LocalSecurityDescriptor const&) = delete;
      LocalSecurityDescriptor& operator=(LocalSecurityDescriptor const&) = delete;
      LocalSecurityDescriptor(LocalSecurityDescriptor&&) = delete;
      LocalSecurityDescriptor& operator=(LocalSecurityDescriptor&&) = delete;

    private:
      PSECURITY_DESCRIPTOR _value = nullptr;
    };

    std::system_error windowsError(DWORD code, char const* operation)
    {
      return std::system_error{static_cast<std::int32_t>(code), std::system_category(), operation};
    }

    std::vector<std::byte> currentUserToken()
    {
      auto token = Handle{};

      if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token.value) == FALSE)
      {
        throw windowsError(::GetLastError(), "OpenProcessToken failed");
      }

      DWORD size = 0;
      auto const firstQuery = ::GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);

      if (firstQuery != FALSE || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER)
      {
        throw windowsError(::GetLastError(), "GetTokenInformation size query failed");
      }

      auto buffer = std::vector<std::byte>(size);

      if (::GetTokenInformation(token.value, TokenUser, buffer.data(), size, &size) == FALSE)
      {
        throw windowsError(::GetLastError(), "GetTokenInformation failed");
      }

      return buffer;
    }

    bool readRestrictionIsEffective(std::filesystem::path const& path)
    {
      auto ec = std::error_code{};
      [[maybe_unused]] auto const iterator = std::filesystem::directory_iterator{path, ec};

      if (!ec)
      {
        return false;
      }

      if (ec == std::errc::permission_denied)
      {
        return true;
      }

      throw std::system_error{ec, "failed to probe denied directory read access"};
    }

    bool writeRestrictionIsEffective(std::filesystem::path const& path)
    {
      auto const probePath =
        path / std::format(L".ao.access-probe.{:08x}.{:016x}", ::GetCurrentProcessId(), ::GetTickCount64());
      auto* const file =
        ::CreateFileW(probePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);

      if (file != INVALID_HANDLE_VALUE)
      {
        ::CloseHandle(file);

        if (::DeleteFileW(probePath.c_str()) == FALSE)
        {
          throw windowsError(::GetLastError(), "failed to remove directory access probe");
        }

        return false;
      }

      auto const error = ::GetLastError();

      if (error == ERROR_ACCESS_DENIED)
      {
        return true;
      }

      throw windowsError(error, "failed to probe denied directory write access");
    }
  } // namespace

  bool hasPrivateManagedFileAccess(std::filesystem::path const& path)
  {
    auto token = currentUserToken();
    auto* const tokenUser = reinterpret_cast<TOKEN_USER*>(token.data());
    auto systemSid = std::array<std::byte, SECURITY_MAX_SID_SIZE>{};

    if (DWORD systemSidSize = static_cast<DWORD>(systemSid.size());
        ::CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid.data(), &systemSidSize) == FALSE)
    {
      throw windowsError(::GetLastError(), "CreateWellKnownSid failed");
    }

    auto nativePath = path.wstring();
    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    PACL dacl = nullptr;
    auto const securityError = ::GetNamedSecurityInfoW(
      nativePath.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &rawDescriptor);

    if (securityError != ERROR_SUCCESS)
    {
      throw windowsError(securityError, "GetNamedSecurityInfoW failed");
    }

    auto descriptor = LocalSecurityDescriptor{rawDescriptor};
    SECURITY_DESCRIPTOR_CONTROL control = 0;

    if (DWORD revision = 0; ::GetSecurityDescriptorControl(rawDescriptor, &control, &revision) == FALSE)
    {
      throw windowsError(::GetLastError(), "GetSecurityDescriptorControl failed");
    }

    if ((control & SE_DACL_PRESENT) == 0 || (control & SE_DACL_PROTECTED) == 0 || dacl == nullptr ||
        dacl->AceCount != 2)
    {
      return false;
    }

    bool userHasFullControl = false;
    bool systemHasFullControl = false;

    for (DWORD index = 0; index < dacl->AceCount; ++index)
    {
      void* rawAccessEntry = nullptr;

      if (::GetAce(dacl, index, &rawAccessEntry) == FALSE)
      {
        throw windowsError(::GetLastError(), "GetAce failed");
      }

      auto const* header = static_cast<ACE_HEADER const*>(rawAccessEntry);

      if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERITED_ACE) != 0)
      {
        return false;
      }

      auto* const accessEntry = static_cast<ACCESS_ALLOWED_ACE*>(rawAccessEntry);

      if ((accessEntry->Mask & FILE_ALL_ACCESS) != FILE_ALL_ACCESS)
      {
        return false;
      }

      PSID const sid = &accessEntry->SidStart;
      auto const currentUser = ::EqualSid(sid, tokenUser->User.Sid) != FALSE;
      auto const localSystem = ::EqualSid(sid, systemSid.data()) != FALSE;

      if (!currentUser && !localSystem)
      {
        return false;
      }

      userHasFullControl |= currentUser;
      systemHasFullControl |= localSystem;
    }

    return userHasFullControl && systemHasFullControl;
  }

  struct ScopedDirectoryAccessGuard::Impl final
  {
    Impl(std::filesystem::path inputPath, DeniedDirectoryAccess inputAccess)
      : path{std::move(inputPath)}, nativePath{path.wstring()}
    {
      auto error = ::GetNamedSecurityInfoW(nativePath.data(),
                                           SE_FILE_OBJECT,
                                           DACL_SECURITY_INFORMATION,
                                           nullptr,
                                           nullptr,
                                           &originalDacl,
                                           nullptr,
                                           &originalSecurityDescriptor);

      if (error != ERROR_SUCCESS)
      {
        throw windowsError(error, "GetNamedSecurityInfoW failed");
      }

      SECURITY_DESCRIPTOR_CONTROL control = 0;

      if (DWORD revision = 0; ::GetSecurityDescriptorControl(originalSecurityDescriptor, &control, &revision) == FALSE)
      {
        error = ::GetLastError();
        releaseSecurityDescriptors();
        throw windowsError(error, "GetSecurityDescriptorControl failed");
      }

      originalDaclProtected = (control & SE_DACL_PROTECTED) != 0;

      try
      {
        auto token = currentUserToken();
        auto* const tokenUser = reinterpret_cast<TOKEN_USER*>(token.data());
        auto access = std::array<EXPLICIT_ACCESSW, 2>{};

        for (auto& entry : access)
        {
          entry.grfAccessMode = DENY_ACCESS;
          entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
          entry.Trustee.TrusteeType = TRUSTEE_IS_USER;
          entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(tokenUser->User.Sid);
        }

        ULONG entryCount = 1;

        if (inputAccess == DeniedDirectoryAccess::Read)
        {
          // Preserve FILE_READ_ATTRIBUTES on the directory itself so callers
          // can identify it before the enumeration attempt fails.
          access[0].grfAccessPermissions = FILE_LIST_DIRECTORY;
          access[0].grfInheritance = NO_INHERITANCE;
          access[1].grfAccessPermissions = FILE_GENERIC_READ;
          access[1].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT | INHERIT_ONLY_ACE;
          entryCount = 2;
        }
        else
        {
          access[0].grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
          access[0].grfInheritance = NO_INHERITANCE;
        }

        error = ::SetEntriesInAclW(entryCount, access.data(), originalDacl, &deniedDacl);

        if (error != ERROR_SUCCESS)
        {
          throw windowsError(error, "SetEntriesInAclW failed");
        }

        error = ::SetNamedSecurityInfoW(nativePath.data(),
                                        SE_FILE_OBJECT,
                                        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                        nullptr,
                                        nullptr,
                                        deniedDacl,
                                        nullptr);

        if (error != ERROR_SUCCESS)
        {
          throw windowsError(error, "SetNamedSecurityInfoW failed");
        }

        applied = true;
        restrictionEffective = inputAccess == DeniedDirectoryAccess::Read ? readRestrictionIsEffective(path)
                                                                          : writeRestrictionIsEffective(path);
      }
      catch (...)
      {
        restore();
        releaseSecurityDescriptors();
        throw;
      }
    }

    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl() noexcept
    {
      restore();
      releaseSecurityDescriptors();
    }

    void restore() noexcept
    {
      if (!applied)
      {
        return;
      }

      auto const protection =
        originalDaclProtected ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
      auto const error = ::SetNamedSecurityInfoW(nativePath.data(),
                                                 SE_FILE_OBJECT,
                                                 DACL_SECURITY_INFORMATION | protection,
                                                 nullptr,
                                                 nullptr,
                                                 originalDacl,
                                                 nullptr);
      applied = false;

      if (error != ERROR_SUCCESS)
      {
        // NOLINTNEXTLINE(modernize-use-std-print): C I/O cannot throw from this noexcept cleanup path.
        std::fprintf(stderr, "Aobus test directory ACL restore failed (error %lu)\n", error);
      }
    }

    void releaseSecurityDescriptors() noexcept
    {
      if (deniedDacl != nullptr)
      {
        ::LocalFree(deniedDacl);
        deniedDacl = nullptr;
      }

      if (originalSecurityDescriptor != nullptr)
      {
        ::LocalFree(originalSecurityDescriptor);
        originalSecurityDescriptor = nullptr;
        originalDacl = nullptr;
      }
    }

    std::filesystem::path path;
    std::wstring nativePath;
    PSECURITY_DESCRIPTOR originalSecurityDescriptor = nullptr;
    PACL originalDacl = nullptr;
    PACL deniedDacl = nullptr;
    bool originalDaclProtected = false;
    bool applied = false;
    bool restrictionEffective = false;
  };

  ScopedDirectoryAccessGuard::ScopedDirectoryAccessGuard(std::filesystem::path path, DeniedDirectoryAccess access)
    : _implPtr{std::make_unique<Impl>(std::move(path), access)}
  {
  }

  ScopedDirectoryAccessGuard::~ScopedDirectoryAccessGuard() noexcept = default;

  bool ScopedDirectoryAccessGuard::effective() const noexcept
  {
    return _implPtr->restrictionEffective;
  }
} // namespace ao::test
