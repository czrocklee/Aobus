# Safe compiler-cache identity for equivalent POSIX workspaces.

if(NOT DEFINED AOBUS_SHARED_WORKSPACES)
  set(AOBUS_SHARED_WORKSPACES OFF)
endif()
set(AOBUS_MANAGED_SHARED_WORKSPACES OFF CACHE BOOL
  "Portal owns the shared-workspace compiler-cache profile")
mark_as_advanced(AOBUS_MANAGED_SHARED_WORKSPACES)

if(AOBUS_SHARED_WORKSPACES)
  if(NOT AOBUS_MANAGED_SHARED_WORKSPACES)
    message(FATAL_ERROR
      "AOBUS_SHARED_WORKSPACES must be configured through ./ao setup compiler-cache "
      "--shared-workspaces and the ./ao portal")
  endif()
  if(WIN32 OR NOT (LINUX OR APPLE))
    message(FATAL_ERROR
      "Shared-workspace compiler caching is supported only on Linux and macOS")
  endif()
  if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR
      "Shared-workspace compiler caching requires the Ninja generator; rerun the portal "
      "command with AOBUS_SHARED_WORKSPACES=0 for this tree")
  endif()
  foreach(_language IN ITEMS C CXX)
    if(NOT CMAKE_${_language}_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
      message(FATAL_ERROR
        "Shared-workspace compiler caching does not support ${CMAKE_${_language}_COMPILER_ID} "
        "for ${_language}; rerun with AOBUS_SHARED_WORKSPACES=0")
    endif()
  endforeach()
  set(_effective_user_flags "${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS}")
  if(CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_type_upper)
    string(APPEND _effective_user_flags
      " ${CMAKE_C_FLAGS_${_build_type_upper}} ${CMAKE_CXX_FLAGS_${_build_type_upper}}")
  endif()
  if(_effective_user_flags MATCHES "-f(file|debug|macro)-prefix-map")
    message(FATAL_ERROR
      "Shared-workspace compiler caching owns all C/C++ prefix maps because ccache does not hash "
      "their values; remove the additional map or rerun with AOBUS_SHARED_WORKSPACES=0")
  endif()
  if(AOBUS_ENABLE_ASAN OR AOBUS_ENABLE_TSAN
     OR _effective_user_flags MATCHES "(--coverage|-fprofile-arcs|-ftest-coverage|-fprofile-instr-generate|-fcoverage-mapping)")
    message(FATAL_ERROR
      "Shared-workspace compiler caching does not support coverage or sanitizer builds; "
      "rerun with AOBUS_SHARED_WORKSPACES=0")
  endif()

  foreach(_name IN ITEMS
      AOBUS_SHARED_WORKSPACE_BUILD_DIR
      AOBUS_SHARED_WORKSPACE_SOURCE_ALIAS
      AOBUS_SHARED_WORKSPACE_MODULE_SHA256
      AOBUS_SHARED_WORKSPACE_POLICY_FINGERPRINT
      AOBUS_SHARED_WORKSPACE_NAMESPACE
      AOBUS_SHARED_WORKSPACE_CCACHE
      AOBUS_SHARED_WORKSPACE_MAP_FILE)
    if(NOT DEFINED ${_name} OR "${${_name}}" STREQUAL "")
      message(FATAL_ERROR "Shared-workspace compiler caching requires ${_name} from the ./ao portal")
    endif()
  endforeach()
  mark_as_advanced(
    AOBUS_SHARED_WORKSPACES
    AOBUS_SHARED_WORKSPACE_BUILD_DIR
    AOBUS_SHARED_WORKSPACE_SOURCE_ALIAS
    AOBUS_SHARED_WORKSPACE_MODULE_SHA256
    AOBUS_SHARED_WORKSPACE_POLICY_FINGERPRINT
    AOBUS_SHARED_WORKSPACE_NAMESPACE
    AOBUS_SHARED_WORKSPACE_CCACHE
    AOBUS_SHARED_WORKSPACE_MAP_FILE)

  if(NOT "${AOBUS_SHARED_WORKSPACE_BUILD_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    message(FATAL_ERROR
      "Shared-workspace build identity '${AOBUS_SHARED_WORKSPACE_BUILD_DIR}' does not match "
      "CMake binary directory '${CMAKE_BINARY_DIR}'")
  endif()
  if(NOT "${AOBUS_SHARED_WORKSPACE_SOURCE_ALIAS}" STREQUAL "${CMAKE_BINARY_DIR}/source"
     OR NOT "${CMAKE_SOURCE_DIR}" STREQUAL "${AOBUS_SHARED_WORKSPACE_SOURCE_ALIAS}")
    message(FATAL_ERROR
      "Shared-workspace source must be configured through the immutable "
      "${CMAKE_BINARY_DIR}/source alias")
  endif()
  if(NOT IS_SYMLINK "${AOBUS_SHARED_WORKSPACE_SOURCE_ALIAS}")
    message(FATAL_ERROR
      "Shared-workspace source alias is missing or is not a symbolic link: "
      "${AOBUS_SHARED_WORKSPACE_SOURCE_ALIAS}")
  endif()
  if(NOT EXISTS "${AOBUS_SHARED_WORKSPACE_MAP_FILE}")
    message(FATAL_ERROR
      "Shared-workspace debugger mapping is missing: ${AOBUS_SHARED_WORKSPACE_MAP_FILE}")
  endif()
  foreach(_digest_name IN ITEMS
      AOBUS_SHARED_WORKSPACE_MODULE_SHA256
      AOBUS_SHARED_WORKSPACE_POLICY_FINGERPRINT)
    string(LENGTH "${${_digest_name}}" _digest_length)
    if(NOT _digest_length EQUAL 64 OR NOT ${_digest_name} MATCHES "^[0-9a-f]+$")
      message(FATAL_ERROR "Shared-workspace ${_digest_name} is not a SHA-256 digest")
    endif()
  endforeach()
  file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _current_module_sha256)
  if(NOT "${AOBUS_SHARED_WORKSPACE_MODULE_SHA256}" STREQUAL "${_current_module_sha256}")
    message(FATAL_ERROR
      "Shared-workspace cache policy changed after this tree was configured; rerun the ./ao "
      "command so the policy fingerprint and namespace can be refreshed")
  endif()

  foreach(_language IN ITEMS C CXX)
    if(NOT AOBUS_MANAGED_${_language}_COMPILER_LAUNCHER
       OR NOT "${CMAKE_${_language}_COMPILER_LAUNCHER}" STREQUAL
              "${AOBUS_SHARED_WORKSPACE_CCACHE}")
      message(FATAL_ERROR
        "Shared-workspace mode requires the verified Aobus-managed ccache ${_language} launcher; "
        "preserve an explicit launcher by rerunning with AOBUS_SHARED_WORKSPACES=0")
    endif()
    set(_launcher "${CMAKE_${_language}_COMPILER_LAUNCHER}")
    list(APPEND _launcher
      "base_dir=${CMAKE_BINARY_DIR}"
      "namespace=${AOBUS_SHARED_WORKSPACE_NAMESPACE}"
      "hash_dir=true"
      "sloppiness=")
    # Deliberately shadow the raw cache value. The portal's ownership sync must
    # continue to compare and persist only the verified ccache executable.
    set(CMAKE_${_language}_COMPILER_LAUNCHER "${_launcher}")
  endforeach()

  add_compile_options(
    "$<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-fdebug-prefix-map=${CMAKE_BINARY_DIR}=/aobus/build>"
    "$<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-ffile-prefix-map=${CMAKE_BINARY_DIR}/=>")
  message(STATUS
    "Shared-workspace compiler caching enabled; debugger source map: "
    "${AOBUS_SHARED_WORKSPACE_MAP_FILE}")
endif()
