# Safe compiler-cache identity for isolated Windows session views.

if(NOT DEFINED AOBUS_WINDOWS_SHARED_WORKSPACES)
  set(AOBUS_WINDOWS_SHARED_WORKSPACES OFF)
endif()
set(AOBUS_MANAGED_WINDOWS_SHARED_WORKSPACES OFF CACHE BOOL
  "Portal owns the Windows shared-workspace compiler-cache profile")
mark_as_advanced(AOBUS_MANAGED_WINDOWS_SHARED_WORKSPACES)

if(AOBUS_WINDOWS_SHARED_WORKSPACES)
  if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC"
     OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR "Windows shared-workspace compiler caching requires native MSVC for C and C++")
  endif()
  if(NOT AOBUS_MANAGED_WINDOWS_SHARED_WORKSPACES)
    message(FATAL_ERROR "Windows shared-workspace compiler caching must be configured through ao.bat")
  endif()
  if(NOT CMAKE_GENERATOR STREQUAL "Ninja" AND NOT CMAKE_GENERATOR MATCHES "^Visual Studio ")
    message(FATAL_ERROR "Windows shared-workspace compiler caching supports Ninja and Visual Studio generators")
  endif()
  foreach(_name IN ITEMS
      AOBUS_WINDOWS_SHARED_WORKSPACE_SOURCE
      AOBUS_WINDOWS_SHARED_WORKSPACE_BUILD
      AOBUS_WINDOWS_SHARED_WORKSPACE_MODULE_SHA256
      AOBUS_WINDOWS_SHARED_WORKSPACE_POLICY_FINGERPRINT
      AOBUS_WINDOWS_SHARED_WORKSPACE_NAMESPACE
      AOBUS_WINDOWS_SHARED_WORKSPACE_CCACHE
      AOBUS_WINDOWS_SHARED_WORKSPACE_MSBUILD_WRAPPER
      AOBUS_WINDOWS_SHARED_WORKSPACE_MAP_FILE)
    if(NOT DEFINED ${_name} OR "${${_name}}" STREQUAL "")
      message(FATAL_ERROR "Windows shared-workspace compiler caching requires ${_name} from ao.bat")
    endif()
  endforeach()
  if(NOT "${CMAKE_SOURCE_DIR}" STREQUAL "${AOBUS_WINDOWS_SHARED_WORKSPACE_SOURCE}"
     OR NOT "${CMAKE_BINARY_DIR}" STREQUAL "${AOBUS_WINDOWS_SHARED_WORKSPACE_BUILD}")
    message(FATAL_ERROR
      "Windows shared-workspace source and build must use the portal-owned S: and B: views")
  endif()
  if(NOT EXISTS "${AOBUS_WINDOWS_SHARED_WORKSPACE_MAP_FILE}")
    message(FATAL_ERROR
      "Windows shared-workspace ownership file is missing: ${AOBUS_WINDOWS_SHARED_WORKSPACE_MAP_FILE}")
  endif()
  foreach(_digest_name IN ITEMS
      AOBUS_WINDOWS_SHARED_WORKSPACE_MODULE_SHA256
      AOBUS_WINDOWS_SHARED_WORKSPACE_POLICY_FINGERPRINT)
    string(LENGTH "${${_digest_name}}" _digest_length)
    if(NOT _digest_length EQUAL 64 OR NOT ${_digest_name} MATCHES "^[0-9a-f]+$")
      message(FATAL_ERROR "Windows shared-workspace ${_digest_name} is not a SHA-256 digest")
    endif()
  endforeach()
  file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _current_module_sha256)
  if(NOT "${AOBUS_WINDOWS_SHARED_WORKSPACE_MODULE_SHA256}" STREQUAL "${_current_module_sha256}")
    message(FATAL_ERROR
      "Windows shared-workspace cache policy changed; rerun ao.bat to refresh its namespace")
  endif()

  add_compile_options(
    "$<$<AND:$<COMPILE_LANGUAGE:C>,$<C_COMPILER_ID:MSVC>>:/experimental:deterministic>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/experimental:deterministic>")
  if(CMAKE_GENERATOR STREQUAL "Ninja")
    foreach(_language IN ITEMS C CXX)
      cmake_path(CONVERT "${CMAKE_${_language}_COMPILER_LAUNCHER}"
        TO_CMAKE_PATH_LIST _configured_launcher NORMALIZE)
      if(NOT AOBUS_MANAGED_${_language}_COMPILER_LAUNCHER
         OR NOT "${_configured_launcher}" STREQUAL
                "${AOBUS_WINDOWS_SHARED_WORKSPACE_CCACHE}")
        message(FATAL_ERROR
          "Windows shared-workspace mode requires the verified Aobus-managed ccache ${_language} launcher")
      endif()
      set(_launcher "${CMAKE_${_language}_COMPILER_LAUNCHER}")
      list(APPEND _launcher
        "base_dir="
        "namespace=${AOBUS_WINDOWS_SHARED_WORKSPACE_NAMESPACE}"
        "hash_dir=true"
        "sloppiness=")
      set(CMAKE_${_language}_COMPILER_LAUNCHER "${_launcher}")
    endforeach()
  else()
    cmake_path(CONVERT "${AOBUS_MSBUILD_CL_TOOL_EXE}" TO_CMAKE_PATH_LIST _configured_wrapper NORMALIZE)
    if(NOT "${_configured_wrapper}" STREQUAL "${AOBUS_WINDOWS_SHARED_WORKSPACE_MSBUILD_WRAPPER}")
      message(FATAL_ERROR
        "Windows shared-workspace mode requires the verified Aobus-managed MSBuild wrapper")
    endif()
  endif()
endif()
