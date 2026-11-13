# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors
"""The canonical VC import spine shared by WinUI SDK query and action projects.

Callers own phase-specific properties, items, targets and forbidden operations.
This module neither selects an SDK nor executes MSBuild.
"""

from __future__ import annotations

import xml.etree.ElementTree as ET
from collections.abc import Mapping, Sequence

_NAMESPACE = "http://schemas.microsoft.com/developer/msbuild/2003"


def _properties(project: ET.Element, values: Mapping[str, str], **attributes: str) -> None:
    group = ET.SubElement(project, "PropertyGroup", attributes)
    for name, value in values.items():
        ET.SubElement(group, name).text = value


def project(
    *,
    configuration: str,
    toolset: str,
    sdk_version: str,
    configuration_type: str,
    initial_properties: Mapping[str, str],
    global_properties: Mapping[str, str],
    project_properties: Mapping[str, str],
    props: Sequence[str],
) -> ET.Element:
    """Build the common prefix through the installed SDK props imports.

    Caller additions retain mapping order: references such as $(IntDir) require
    the defining property to precede its consumer within the same group.
    """
    root = ET.Element("Project", {"xmlns": _NAMESPACE})
    _properties(
        root,
        {
            "Configuration": configuration,
            "Platform": "x64",
            "ImportDirectoryBuildProps": "false",
            "ImportDirectoryBuildTargets": "false",
            # Only explicit SDK imports and the declared installed VC spine
            # belong to these private projects, not user/machine extensions.
            "ImportUserLocationsByWildcardBeforeMicrosoftCommonProps": "false",
            "ImportUserLocationsByWildcardAfterMicrosoftCommonProps": "false",
            "ImportUserLocationsByWildcardBeforeMicrosoftCommonTargets": "false",
            "ImportUserLocationsByWildcardAfterMicrosoftCommonTargets": "false",
            "ImportByWildcardBeforeMicrosoftCommonProps": "false",
            "ImportByWildcardAfterMicrosoftCommonProps": "false",
            "ImportByWildcardBeforeMicrosoftCommonTargets": "false",
            "ImportByWildcardAfterMicrosoftCommonTargets": "false",
            "ImportProjectExtensionProps": "false",
            "ImportProjectExtensionTargets": "false",
            # Empty Common custom hooks select a machine-wide default instead
            # of disabling the import. Keep their absent paths project-local.
            "CustomBeforeMicrosoftCommonProps": "$(MSBuildProjectFullPath).no-custom-imports",
            "CustomAfterMicrosoftCommonProps": "$(MSBuildProjectFullPath).no-custom-imports",
            "CustomBeforeMicrosoftCommonTargets": "$(MSBuildProjectFullPath).no-custom-imports",
            "CustomAfterMicrosoftCommonTargets": "$(MSBuildProjectFullPath).no-custom-imports",
            "CustomBeforeDirectoryBuildProps": "",
            "CustomAfterDirectoryBuildProps": "",
            "CustomBeforeDirectoryBuildTargets": "",
            "CustomAfterDirectoryBuildTargets": "",
            "ForceImportBeforeCppProps": "",
            "ForceImportAfterCppProps": "",
            "ForceImportAfterCppDefaultProps": "",
            "ForceImportBeforeCppTargets": "",
            "ForceImportAfterCppTargets": "",
            **initial_properties,
        },
    )
    configurations = ET.SubElement(root, "ItemGroup", Label="ProjectConfigurations")
    entry = ET.SubElement(configurations, "ProjectConfiguration", Include=configuration + "|x64")
    ET.SubElement(entry, "Configuration").text = configuration
    ET.SubElement(entry, "Platform").text = "x64"
    _properties(
        root,
        {
            "RootNamespace": "Aobus",
            "TargetName": "Aobus",
            "DefaultLanguage": "en",
            "ApplicationType": "Windows Store",
            "ApplicationTypeRevision": "10.0",
            "WindowsTargetPlatformMinVersion": sdk_version,
            **global_properties,
        },
        Label="Globals",
    )
    ET.SubElement(root, "Import", Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props")
    _properties(
        root,
        {
            "ConfigurationType": configuration_type,
            "PlatformToolset": toolset,
            "CharacterSet": "Unicode",
            "WindowsTargetPlatformVersion": sdk_version,
            "UseDebugLibraries": str(configuration == "Debug").lower(),
        },
        Label="Configuration",
    )
    ET.SubElement(root, "Import", Project="$(VCTargetsPath)\\Microsoft.Cpp.props")
    _properties(
        root,
        {
            "OutputType": "WinExe",
            "UseWinUI": "true",
            "OutDir": "$(ProjectDir)out\\",
            "WindowsAppContainer": "false",
            "AppxPackage": "false",
            "WindowsPackageType": "None",
            "WindowsAppSDKSelfContained": "false",
            "CppWinRTOptimized": "true",
            "CppWinRTRootNamespaceAutoMerge": "true",
            "MinimalCoreWin": "true",
            **project_properties,
        },
    )
    for path in props:
        ET.SubElement(root, "Import", Project=path)
    return root


def finish(project: ET.Element, targets: Sequence[str]) -> None:
    """Append native VC and installed SDK targets after the caller-owned items."""
    ET.SubElement(project, "Import", Project="$(VCTargetsPath)\\Microsoft.Cpp.targets")
    for path in targets:
        ET.SubElement(project, "Import", Project=path)
