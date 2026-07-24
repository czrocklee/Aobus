# Windows App SDK NuGet restore and generated Visual Studio/MSBuild imports.

function(_aobus_read_nuget_lock output)
  file(STRINGS "${AOBUS_WINDOWS_APP_SDK_LOCK}" package_lines
    REGEX "<package id=\"[^\"]+\" version=\"[^\"]+\"")
  set(packages)
  foreach(line IN LISTS package_lines)
    string(REGEX MATCH "id=\"([^\"]+)\"" ignored "${line}")
    set(package_id "${CMAKE_MATCH_1}")
    string(REGEX MATCH "version=\"([^\"]+)\"" ignored "${line}")
    set(package_version "${CMAKE_MATCH_1}")
    if(NOT package_id OR NOT package_version)
      message(FATAL_ERROR
        "Invalid NuGet package entry in ${AOBUS_WINDOWS_APP_SDK_LOCK}: ${line}")
    endif()
    list(APPEND packages "${package_id}|${package_version}")
  endforeach()
  if(NOT packages)
    message(FATAL_ERROR
      "Windows App SDK lock contains no packages: ${AOBUS_WINDOWS_APP_SDK_LOCK}")
  endif()
  set(${output} "${packages}" PARENT_SCOPE)
endfunction()

function(_aobus_nuget_package_fields package output_id output_version)
  string(REPLACE "|" ";" fields "${package}")
  list(GET fields 0 package_id)
  list(GET fields 1 package_version)
  set(${output_id} "${package_id}" PARENT_SCOPE)
  set(${output_version} "${package_version}" PARENT_SCOPE)
endfunction()

function(aobus_nuget_locked_version requested_id output)
  _aobus_read_nuget_lock(packages)
  foreach(package IN LISTS packages)
    _aobus_nuget_package_fields("${package}" package_id package_version)
    if(package_id STREQUAL requested_id)
      set(${output} "${package_version}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR
    "NuGet lock ${AOBUS_WINDOWS_APP_SDK_LOCK} is missing ${requested_id}.")
endfunction()

function(_aobus_nuget_restore_complete output packages_dir packages)
  foreach(package IN LISTS packages)
    _aobus_nuget_package_fields("${package}" package_id package_version)
    set(package_dir "${packages_dir}/${package_id}.${package_version}")
    file(GLOB archives "${package_dir}/*.nupkg")
    list(LENGTH archives archive_count)
    if(NOT IS_DIRECTORY "${package_dir}" OR NOT archive_count EQUAL 1)
      set(${output} FALSE PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${output} TRUE PARENT_SCOPE)
endfunction()

function(aobus_restore_windows_app_sdk)
  if(NOT WIN32)
    message(FATAL_ERROR "AOBUS_BUILD_WINUI requires a native Windows host.")
  endif()
  if(NOT CMAKE_GENERATOR STREQUAL "Visual Studio 18 2026")
    message(FATAL_ERROR
      "AOBUS_BUILD_WINUI requires the 'Visual Studio 18 2026' generator; "
      "use the windows-winui preset.")
  endif()
  if(NOT EXISTS "${AOBUS_WINDOWS_APP_SDK_LOCK}"
     OR NOT EXISTS "${AOBUS_WINDOWS_APP_SDK_NUGET_CONFIG}")
    message(FATAL_ERROR "Windows App SDK NuGet lock/configuration is incomplete.")
  endif()

  file(SHA256 "${AOBUS_WINDOWS_APP_SDK_LOCK}" lock_hash)
  if(NOT AOBUS_NUGET_PACKAGES_DIR)
    if(DEFINED ENV{AOBUS_STATE_ROOT} AND NOT "$ENV{AOBUS_STATE_ROOT}" STREQUAL "")
      set(state_root "$ENV{AOBUS_STATE_ROOT}")
    elseif(DEFINED ENV{LOCALAPPDATA} AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
      set(state_root "$ENV{LOCALAPPDATA}/Aobus")
    else()
      message(FATAL_ERROR
        "AOBUS_BUILD_WINUI requires LOCALAPPDATA or AOBUS_STATE_ROOT for the NuGet cache.")
    endif()
    file(TO_CMAKE_PATH "${state_root}" state_root)
    string(SUBSTRING "${lock_hash}" 0 12 lock_id)
    set(AOBUS_NUGET_PACKAGES_DIR
      "${state_root}/n/${lock_id}/p"
      CACHE PATH "Host-local exact NuGet package directory for WinUI")
  endif()

  _aobus_read_nuget_lock(packages)
  get_filename_component(cache_root "${AOBUS_NUGET_PACKAGES_DIR}" DIRECTORY)
  file(MAKE_DIRECTORY "${cache_root}")
  file(LOCK "${cache_root}/restore.lock" GUARD FUNCTION TIMEOUT 600)

  set(marker "${cache_root}/.aobus-nuget-complete")
  set(marker_matches FALSE)
  if(EXISTS "${marker}")
    file(READ "${marker}" marker_value)
    string(STRIP "${marker_value}" marker_value)
    if(marker_value STREQUAL lock_hash)
      set(marker_matches TRUE)
    endif()
  endif()
  _aobus_nuget_restore_complete(packages_complete "${AOBUS_NUGET_PACKAGES_DIR}" "${packages}")
  if(marker_matches AND packages_complete)
    set(AOBUS_NUGET_PACKAGES_DIR "${AOBUS_NUGET_PACKAGES_DIR}" PARENT_SCOPE)
    set(AOBUS_WINDOWS_APP_SDK_LOCK_SHA256 "${lock_hash}" PARENT_SCOPE)
    return()
  endif()

  find_program(AOBUS_VCPKG_EXECUTABLE
    NAMES vcpkg.exe vcpkg
    HINTS "$ENV{VCPKG_ROOT}"
    REQUIRED)
  execute_process(
    COMMAND "${AOBUS_VCPKG_EXECUTABLE}" fetch nuget
    RESULT_VARIABLE fetch_status
    OUTPUT_VARIABLE fetch_output
    ERROR_VARIABLE fetch_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT fetch_status EQUAL 0)
    message(FATAL_ERROR "vcpkg failed to provision NuGet: ${fetch_error}")
  endif()
  string(REPLACE "\r\n" "\n" fetch_output "${fetch_output}")
  string(REPLACE "\n" ";" fetch_lines "${fetch_output}")
  list(FILTER fetch_lines EXCLUDE REGEX "^$")
  list(GET fetch_lines -1 nuget_executable)
  if(NOT EXISTS "${nuget_executable}")
    message(FATAL_ERROR
      "vcpkg did not report a usable NuGet executable: ${fetch_output}")
  endif()

  file(MAKE_DIRECTORY "${AOBUS_NUGET_PACKAGES_DIR}")
  execute_process(
    COMMAND "${nuget_executable}" install "${AOBUS_WINDOWS_APP_SDK_LOCK}"
      -OutputDirectory "${AOBUS_NUGET_PACKAGES_DIR}"
      -ConfigFile "${AOBUS_WINDOWS_APP_SDK_NUGET_CONFIG}"
      -DirectDownload
      -NonInteractive
      -Verbosity normal
    RESULT_VARIABLE restore_status
    OUTPUT_VARIABLE restore_output
    ERROR_VARIABLE restore_error)
  if(NOT restore_status EQUAL 0)
    message(FATAL_ERROR
      "NuGet restore failed for ${AOBUS_WINDOWS_APP_SDK_LOCK}:\n"
      "${restore_output}\n${restore_error}")
  endif()

  _aobus_nuget_restore_complete(packages_complete "${AOBUS_NUGET_PACKAGES_DIR}" "${packages}")
  if(NOT packages_complete)
    message(FATAL_ERROR
      "NuGet restore completed without the exact Windows App SDK package closure.")
  endif()
  file(WRITE "${marker}" "${lock_hash}\n")
  set(AOBUS_NUGET_PACKAGES_DIR "${AOBUS_NUGET_PACKAGES_DIR}" PARENT_SCOPE)
  set(AOBUS_WINDOWS_APP_SDK_LOCK_SHA256 "${lock_hash}" PARENT_SCOPE)
endfunction()

function(_aobus_xml_escape output value)
  string(REPLACE "&" "&amp;" escaped "${value}")
  string(REPLACE "\"" "&quot;" escaped "${escaped}")
  string(REPLACE "<" "&lt;" escaped "${escaped}")
  string(REPLACE ">" "&gt;" escaped "${escaped}")
  set(${output} "${escaped}" PARENT_SCOPE)
endfunction()

function(_aobus_nuget_import package extension output)
  _aobus_nuget_package_fields("${package}" package_id package_version)
  set(package_dir "${AOBUS_NUGET_PACKAGES_DIR}/${package_id}.${package_version}")
  set(native_import "${package_dir}/build/native/${package_id}.${extension}")
  set(generic_import "${package_dir}/build/${package_id}.${extension}")
  if(EXISTS "${native_import}")
    set(import_path "${native_import}")
  elseif(EXISTS "${generic_import}")
    set(import_path "${generic_import}")
  else()
    set(${output} "" PARENT_SCOPE)
    return()
  endif()
  file(TO_CMAKE_PATH "${import_path}" import_path)
  _aobus_xml_escape(import_path "${import_path}")
  set(${output}
    "  <Import Project=\"${import_path}\" Condition=\"Exists('${import_path}')\" />\n"
    PARENT_SCOPE)
endfunction()

function(aobus_enable_windows_app_sdk target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "Windows App SDK target does not exist: ${target}")
  endif()
  if(NOT AOBUS_NUGET_PACKAGES_DIR)
    message(FATAL_ERROR "Call aobus_restore_windows_app_sdk before configuring ${target}.")
  endif()

  _aobus_read_nuget_lock(packages)
  set(props_imports)
  foreach(package IN LISTS packages)
    _aobus_nuget_import("${package}" props import)
    string(APPEND props_imports "${import}")
  endforeach()

  set(target_packages "${packages}")
  list(REVERSE target_packages)
  set(targets_imports)
  foreach(package IN LISTS target_packages)
    _aobus_nuget_import("${package}" targets import)
    string(APPEND targets_imports "${import}")
  endforeach()

  set(props_file "${CMAKE_CURRENT_BINARY_DIR}/Aobus.WinUI.props")
  set(targets_file "${CMAKE_CURRENT_BINARY_DIR}/Aobus.WinUI.targets")
  file(WRITE "${props_file}"
    "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
    "  <PropertyGroup>\n"
    "    <OutputType>WinExe</OutputType>\n"
    "    <UseWinUI>true</UseWinUI>\n"
    "    <WindowsAppContainer>false</WindowsAppContainer>\n"
    "    <AppxPackage>false</AppxPackage>\n"
    "    <WindowsPackageType>None</WindowsPackageType>\n"
    "    <WindowsAppSDKSelfContained>false</WindowsAppSDKSelfContained>\n"
    "    <CppWinRTOptimized>true</CppWinRTOptimized>\n"
    "    <CppWinRTRootNamespaceAutoMerge>true</CppWinRTRootNamespaceAutoMerge>\n"
    "    <MinimalCoreWin>true</MinimalCoreWin>\n"
    "  </PropertyGroup>\n"
    "${props_imports}"
    "  <PropertyGroup>\n"
    "    <CharacterSet>Unicode</CharacterSet>\n"
    "    <WindowsAppContainer>false</WindowsAppContainer>\n"
    "    <AppxPackage>false</AppxPackage>\n"
    "    <WindowsPackageType>None</WindowsPackageType>\n"
    "  </PropertyGroup>\n"
    "</Project>\n")
  file(WRITE "${targets_file}"
    "<Project xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
    "${targets_imports}"
    "  <PropertyGroup>\n"
    "    <CharacterSet>Unicode</CharacterSet>\n"
    "    <WindowsAppContainer>false</WindowsAppContainer>\n"
    "    <AppxPackage>false</AppxPackage>\n"
    "    <WindowsPackageType>None</WindowsPackageType>\n"
    "  </PropertyGroup>\n"
    "  <ItemDefinitionGroup>\n"
    "    <Midl>\n"
    "      <OutputDirectory>$(ProjectDir)</OutputDirectory>\n"
    "    </Midl>\n"
    "    <Link>\n"
    "      <AdditionalDependencies>"
    "$(_FoundationLibFolder)\\Microsoft.WindowsAppRuntime.Bootstrap.lib;"
    "$(_FoundationLibFolder)\\Microsoft.WindowsAppRuntime.lib;"
    "%(AdditionalDependencies)</AdditionalDependencies>\n"
    "    </Link>\n"
    "  </ItemDefinitionGroup>\n"
    "  <Target Name=\"AobusNormalizeMidlOutputDirectory\"\n"
    "          BeforeTargets=\"Midl;GetCppWinRTMdMergeInputs\">\n"
    "    <ItemGroup>\n"
    "      <Midl Update=\"@(Midl)\">\n"
    "        <OutputDirectory>$(ProjectDir)</OutputDirectory>\n"
    "      </Midl>\n"
    "    </ItemGroup>\n"
    "  </Target>\n"
    "  <ItemGroup>\n"
    "    <ClCompile Include=\"$(GeneratedFilesDir)module.g.cpp\" />\n"
    "  </ItemGroup>\n"
    "</Project>\n")

  configure_file(
    "${AOBUS_WINDOWS_APP_SDK_LOCK}"
    "${CMAKE_CURRENT_BINARY_DIR}/packages.config"
    COPYONLY)

  set_target_properties("${target}" PROPERTIES
    VS_GLOBAL_ForceImportAfterCppProps "${props_file}"
    VS_GLOBAL_ForceImportAfterCppTargets "${targets_file}")
endfunction()
