# Persist the ordinary Debug policy and Nix compiler-cache identity for direct
# Ninja/Make and IDE builds. The raw launcher cache remains owned by the portal
# or the developer.
function(aobus_configure_nix_hardening)
  if(NOT LINUX OR NOT DEFINED ENV{AOBUS_NIX_DEPENDENCY_REPORT})
    return()
  endif()

  # Every Nix store file has mtime 1 and same-template wrappers can share a
  # size, so ccache's default mtime check can reuse another pinned compiler's
  # objects. Hash the managed ccache compiler by content in every configuration;
  # sccache already keys compilers by executable content.
  foreach(language IN ITEMS C CXX)
    set(raw "${CMAKE_${language}_COMPILER_LAUNCHER}")
    if(AOBUS_MANAGED_${language}_COMPILER_LAUNCHER AND NOT raw STREQUAL "")
      list(GET raw 0 executable)
      get_filename_component(name "${executable}" NAME)
      if(name STREQUAL "ccache" AND NOT executable MATCHES "[$]<")
        list(APPEND raw "compiler_check=content")
        set(CMAKE_${language}_COMPILER_LAUNCHER "${raw}")
        set(CMAKE_${language}_COMPILER_LAUNCHER "${raw}" PARENT_SCOPE)
      endif()
    endif()
  endforeach()

  if(AOBUS_ENABLE_ASAN OR AOBUS_ENABLE_TSAN)
    return()
  endif()

  set(report "$ENV{AOBUS_NIX_DEPENDENCY_REPORT}")
  file(READ "${report}" metadata)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${report}")
  string(JSON schema GET "${metadata}" schemaVersion)
  if(NOT schema EQUAL 1)
    message(FATAL_ERROR "Unsupported Nix dependency report schema: ${schema}")
  endif()
  string(JSON count LENGTH "${metadata}" ccWrapper hardeningEnable)
  set(debug_hardening)
  if(count GREATER 0)
    math(EXPR last "${count} - 1")
    foreach(index RANGE 0 ${last})
      string(JSON flag GET "${metadata}" ccWrapper hardeningEnable ${index})
      if(NOT flag MATCHES "^[a-zA-Z0-9_]+$")
        message(FATAL_ERROR "Invalid Nix hardening policy token: ${flag}")
      endif()
      if(NOT flag MATCHES "^fortify(3)?$")
        list(APPEND debug_hardening "${flag}")
      endif()
    endforeach()
  endif()
  string(JOIN " " policy ${debug_hardening})

  # Normal shell wrappers append generic and role channels to the salted one.
  # Normalize all of them, rather than allowing a role to reintroduce fortify.
  set(channels NIX_HARDENING_ENABLE NIX_HARDENING_ENABLE_FOR_BUILD NIX_HARDENING_ENABLE_FOR_TARGET)
  foreach(field IN ITEMS suffixSalt gccSuffixSalt)
    string(JSON salt GET "${metadata}" ccWrapper ${field})
    if(NOT salt MATCHES "^[a-zA-Z0-9_]+$")
      message(FATAL_ERROR "Invalid Nix compiler-wrapper suffix salt: ${salt}")
    endif()
    list(APPEND channels "NIX_HARDENING_ENABLE_${salt}")
  endforeach()
  list(REMOVE_DUPLICATES channels)
  set(assignments)
  foreach(channel IN LISTS channels)
    list(APPEND assignments "${channel}=${policy}")
  endforeach()
  # Neither cache sees the wrapper's environment, so a policy change would
  # otherwise reuse objects from an identical command line. Add the exact
  # assignments to the caller's cache inputs for Debug objects; never replace them.
  set(policy_file "${CMAKE_BINARY_DIR}/nix-debug-hardening.policy")
  string(JOIN "\n" policy_text ${assignments})
  file(CONFIGURE OUTPUT "${policy_file}" CONTENT "${policy_text}\n" @ONLY NEWLINE_STYLE UNIX)
  file(SHA256 "${policy_file}" policy_digest)
  foreach(language IN ITEMS C CXX)
    # A normal variable shadows, but never overwrites, the original cache list.
    # Other configurations keep their launcher and the shell's ambient policy.
    set(raw "${CMAKE_${language}_COMPILER_LAUNCHER}")
    set(configured "")
    if(NOT raw STREQUAL "")
      list(GET raw 0 executable)
      get_filename_component(name "${executable}" NAME)
      if(name STREQUAL "ccache" AND NOT executable MATCHES "[$]<")
        # Only query a concrete executable, never a generator-expression fragment.
        # CCACHE_EXTRAFILES replaces configured extra files, so carry them over;
        # every portal build reconfigures.
        execute_process(
          COMMAND "${CMAKE_COMMAND}" -E env --unset=CCACHE_EXTRAFILES
            "${executable}" --get-config extra_files_to_hash
          WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
          RESULT_VARIABLE status
          OUTPUT_VARIABLE configured
          ERROR_VARIABLE error
          OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT status EQUAL 0)
          message(FATAL_ERROR
            "Cannot read the extra files ${executable} is configured to hash: ${status}\n${error}")
        endif()
      endif()
    endif()
    # Modifications apply to the build-time environment, including direct
    # Ninja/Make and IDE builds, so a caller's value stays part of the key.
    set(prefix "${CMAKE_COMMAND}" -E env ${assignments})
    if(NOT configured STREQUAL "")
      # ccache uses colons, not semicolons, as Linux path-list separators.
      # Preserve the complete configured value as one CMake argument.
      string(REPLACE ";" "\\;" configured "${configured}")
      list(APPEND prefix --modify "CCACHE_EXTRAFILES=path_list_append:${configured}")
    endif()
    list(APPEND prefix --modify "CCACHE_EXTRAFILES=path_list_append:${policy_file}")
    list(APPEND prefix --modify "SCCACHE_C_CUSTOM_CACHE_BUSTER=string_append:${policy_digest}" --)
    # Evaluate conditional launchers and settings before recognizing ccache and
    # appending its authoritative CLI input. IF short-circuits an empty launcher.
    # Nest literal text inside $<1:...> so commas remain content, not parameters.
    set(launcher "$<1:${raw}>")
    set(nonempty "$<NOT:$<STREQUAL:${launcher},>>")
    set(is_ccache
      "$<IF:${nonempty},$<STREQUAL:$<PATH:GET_FILENAME,$<LIST:GET,${launcher},0>>,ccache>,0>")
    set(with_policy
      "$<LIST:TRANSFORM,${launcher},APPEND,:$<1:${policy_file}>,REGEX,^extra_files_to_hash=.+$>")
    set(with_policy
      "$<LIST:TRANSFORM,${with_policy},APPEND,$<1:${policy_file}>,REGEX,^extra_files_to_hash=$>")
    set(launcher "$<IF:$<AND:$<CONFIG:Debug>,${is_ccache}>,${with_policy},${launcher}>")
    set(separator "$<${nonempty}:;>")
    set(CMAKE_${language}_COMPILER_LAUNCHER
      "$<$<CONFIG:Debug>:${prefix}${separator}>${launcher}" PARENT_SCOPE)
  endforeach()
endfunction()
