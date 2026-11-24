# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Aobus Contributors

param(
    [Parameter(Mandatory = $true)][string]$AppExe,
    [Parameter(Mandatory = $true)][string]$SeedExe
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class AlbumColumnsWindow {
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
}
'@
if ([Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0) {
    throw 'Run the Album Columns probe on the interactive desktop, not in session 0'
}
foreach ($file in @($AppExe, $SeedExe)) {
    if (-not [IO.Path]::IsPathRooted($file) -or -not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Expected an absolute path to a staged executable: $file"
    }
}
$exe = $AppExe
$probe = $SeedExe
$ae = [System.Windows.Automation.AutomationElement]
$ct = [System.Windows.Automation.ControlType]
$pattern = [System.Windows.Automation.TogglePattern]::Pattern
$invoke = [System.Windows.Automation.InvokePattern]::Pattern
$scope = [System.Windows.Automation.TreeScope]::Descendants

function Find-Native($root, $property, $value, $role) {
    if ($null -eq $root) { return $null }
    $condition = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition($property, $value)),
        (New-Object System.Windows.Automation.PropertyCondition($ae::ControlTypeProperty, $role)),
        (New-Object System.Windows.Automation.PropertyCondition($ae::IsOffscreenProperty, $false)))
    return $root.FindFirst($scope, $condition)
}

function Wait-Native([string]$description, [scriptblock]$sample) {
    $deadline = (Get-Date).AddSeconds(14)
    do {
        try { $found = & $sample } catch [System.Windows.Automation.ElementNotAvailableException] { $found = $null }
        if ($found) { return $found }
        Start-Sleep -Milliseconds 180
    } while ((Get-Date) -lt $deadline)
    throw "$description not exposed by the owned WinUI process"
}

function Find-AppControl([string]$name, $role) {
    foreach ($window in (Get-ProbeWindows)) {
        $element = Find-Native $window $ae::NameProperty $name $role
        if ($element) { return $element }
    }
    return $null
}

function Require-AppControl([string]$name, $role) {
    Wait-Native $name { Find-AppControl $name $role }
}

function Get-Main {
    $ae::FromHandle($script:AoHandle)
}

function Get-Presentation {
    Find-Native (Get-Main) $ae::AutomationIdProperty 'modern-presentation-button' $ct::Button
}

function Require-Albums {
    $null = Wait-Native 'Albums selection' {
        $p = Get-Presentation
        if ($p -and $p.Current.Name -eq 'Albums') { $p }
    }
}

function Note([string]$message) { Write-Host $message }

function Get-ProbeWindows {
    $byProcess = New-Object System.Windows.Automation.PropertyCondition($ae::ProcessIdProperty, $script:AoProcessId)
    $ae::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children, $byProcess)
}

function Close($process) {
    if (-not $process) { return }
    if (-not $process.HasExited) {
        $null = $process.CloseMainWindow()
        if (-not $process.WaitForExit(30000)) {
            throw "Owned Aobus process $($process.Id) did not close; it was not terminated"
        }
    }
    Note "closed=True exit=$($process.ExitCode)"
    $script:AoHandle = [IntPtr]::Zero
    $script:AoProcessId = 0
}

function Start-IsolatedApp([string]$arguments) {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $exe
    $start.Arguments = $arguments
    $start.WorkingDirectory = Split-Path $exe
    $start.UseShellExecute = $false
    $start.EnvironmentVariables['LOCALAPPDATA'] = $privateProfile
    $start.EnvironmentVariables['APPDATA'] = $privateProfile
    $p = [System.Diagnostics.Process]::Start($start)
    $script:ownedProcess = $p
    $null = $p.Handle
    $deadline = (Get-Date).AddSeconds(60)
    do {
        if ($p.HasExited) { throw "WinUI exited at launch with code $($p.ExitCode)" }
        $p.Refresh()
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
            $script:AoHandle = $p.MainWindowHandle
            $script:AoProcessId = $p.Id
            # Give the owned window room for the sidebar and keep menu popups in the foreground.
            [void][AlbumColumnsWindow]::ShowWindow($script:AoHandle, 3)
            [void][AlbumColumnsWindow]::SetForegroundWindow($script:AoHandle)
            Note "launch=ok handle=$script:AoHandle pid=$script:AoProcessId"
            return $p
        }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    Note "launch=timeout left-running pid=$($p.Id)"
    return $p
}

function Assert-AlbumBody([bool]$visible, [string]$stage) {
    Require-Albums
    $row = Require-AppControl 'Audit Alpha' $ct::ListItem
    $null = Require-AppControl 'Audit Album' $ct::ListItem
    if ($visible) {
        $null = Require-AppControl 'Album' $ct::Button
        $null = Wait-Native 'Audit Album text inside the Audit Alpha ListItem' {
            Find-Native (Find-AppControl 'Audit Alpha' $ct::ListItem) $ae::NameProperty 'Audit Album' $ct::Text
        }
    } else {
        if ((Find-AppControl 'Album' $ct::Button) -or (Find-Native $row $ae::NameProperty 'Audit Album' $ct::Text)) {
            throw "Unexpected row Album header or cell at $stage"
        }
    }
    Note "album-body=$stage grouped=True column-visible=$visible"
}

function Open-Columns {
    $more = Require-AppControl 'More' $ct::Button
    $more.GetCurrentPattern($invoke).Invoke()
    (Require-AppControl 'Columns...' $ct::MenuItem).GetCurrentPattern($invoke).Invoke()
    Require-AppControl 'Album' $ct::MenuItem
}

function Assert-Toggle($item, [bool]$expected, [string]$stage) {
    $value = $item.GetCurrentPattern($pattern).Current.ToggleState
    if ($value.ToString() -ne $(if ($expected) { 'On' } else { 'Off' })) {
        throw "Toggle '$($item.Current.Name)' at $stage was $value; expected checked=$expected"
    }
    Note "column-toggle=$stage name=$($item.Current.Name) checked=$expected enabled=$($item.Current.IsEnabled)"
}

function Invoke-Toggle($item) {
    $provider = $null
    if ($item.TryGetCurrentPattern($invoke, [ref]$provider)) {
        $provider.Invoke()
    } else {
        $item.GetCurrentPattern($pattern).Toggle()
    }
}

function Select-List([string]$name) {
    $selection = [System.Windows.Automation.SelectionItemPattern]::Pattern
    $item = Wait-Native "sidebar $name" {
        $navigation = Get-Main
        $id = New-Object System.Windows.Automation.PropertyCondition($ae::AutomationIdProperty, 'modern-navigation')
        $pane = $navigation.FindFirst($scope, $id)
        if ($pane) {
            $condition = New-Object System.Windows.Automation.PropertyCondition($ae::NameProperty, $name)
            $pane.FindFirst($scope, $condition)
        }
    }
    $item.GetCurrentPattern($selection).Select()
    $null = Wait-Native "selected sidebar $name" {
        if ($item.GetCurrentPattern($selection).Current.IsSelected) { $item }
    }
    Note "sidebar-selected=$name"
}

function Assert-LastVisible($album) {
    $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    $menu = $walker.GetParent($album)
    $moreChecked = @()
    for ($item = $walker.GetFirstChild($menu); $item; $item = $walker.GetNextSibling($item)) {
        $provider = $null
        if ($item.Current.Name -ne 'Title' -and $item.TryGetCurrentPattern($pattern, [ref]$provider) -and
            $provider.Current.ToggleState.ToString() -eq 'On') {
            $moreChecked += $item.Current.Name
        }
    }
    if ($moreChecked.Count -eq 0) { throw 'Could not enumerate the other initially visible columns' }
    foreach ($name in $moreChecked) {
        $item = Require-AppControl $name $ct::MenuItem
        Assert-Toggle $item $true 'before-removing-other-column'
        Invoke-Toggle $item
        $null = Open-Columns
    }
    $title = Require-AppControl 'Title' $ct::MenuItem
    Assert-Toggle $title $true 'last-visible-before'
    if ($title.Current.IsEnabled) {
        Invoke-Toggle $title
        $null = Open-Columns
        $title = Require-AppControl 'Title' $ct::MenuItem
    }
    Assert-Toggle $title $true 'last-visible-after'
    foreach ($name in $moreChecked) {
        Assert-Toggle (Require-AppControl $name $ct::MenuItem) $false 'last-visible-others'
    }
    $null = Require-AppControl 'Title' $ct::Button
    $null = Require-AppControl 'Audit Alpha' $ct::ListItem
    Note 'last-visible-guard=Title retained'
}

function Select-Presentation([string]$name) {
    (Get-Presentation).GetCurrentPattern($invoke).Invoke()
    (Require-AppControl $name $ct::MenuItem).GetCurrentPattern($invoke).Invoke()
    $null = Wait-Native "presentation $name" {
        $presentation = Get-Presentation
        if ($presentation -and $presentation.Current.Name -eq $name) { $presentation }
    }
}

function Prepare-NoVisibleColumns {
    # The earlier guard check leaves only Title visible and the menu open.
    Invoke-Toggle (Require-AppControl 'Album' $ct::MenuItem)
    $null = Open-Columns
    Invoke-Toggle (Require-AppControl 'Title' $ct::MenuItem)
    Select-Presentation 'Library'
    $album = Open-Columns
    Assert-Toggle $album $true 'Library-before-hiding-Album'
    Assert-Toggle (Require-AppControl 'Year' $ct::MenuItem) $true 'Library-retains-Year'
    Invoke-Toggle $album
    $null = Wait-Native 'Library showing only Year' {
        if ((Find-AppControl 'Year' $ct::Button) -and -not (Find-AppControl 'Album' $ct::Button)) { $true }
    }
    Select-Presentation 'Albums'
}

function Get-LayoutReceipt([string]$path) {
    $file = Get-Item -LiteralPath $path
    return "$($file.LastWriteTimeUtc.Ticks):$((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash)"
}

function Check-NoVisibleColumnsRecovery([string]$layoutFile) {
    Require-Albums
    $tableId = New-Object System.Windows.Automation.PropertyCondition($ae::AutomationIdProperty, 'modern-track-table')
    $table = Wait-Native 'owned modern track table' { (Get-Main).FindFirst($scope, $tableId) }
    $message = 'No track columns are visible in this presentation.'
    $null = Wait-Native 'no visible columns message' { Find-Native $table $ae::NameProperty $message $ct::Text }
    $null = Require-AppControl 'Audit Album' $ct::ListItem
    $null = Require-AppControl 'Audit Alpha' $ct::ListItem
    $before = Get-LayoutReceipt $layoutFile
    $button = Wait-Native 'track table Columns recovery action' {
        Find-Native $table $ae::NameProperty 'Columns...' $ct::Button
    }
    $button.GetCurrentPattern($invoke).Invoke()
    foreach ($name in @('Track #', 'Title', 'Artist', 'Duration', 'Album', 'Album Artist')) {
        Assert-Toggle (Require-AppControl $name $ct::MenuItem) $false 'recovery-all-unchecked'
    }
    if ((Get-LayoutReceipt $layoutFile) -ne $before) { throw 'Opening Columns recovery rewrote the saved layout' }
    Invoke-Toggle (Require-AppControl 'Title' $ct::MenuItem)
    $null = Require-AppControl 'Title' $ct::Button
    $null = Wait-Native 'recovery message retired when Title is shown' {
        if (-not (Find-Native $table $ae::NameProperty $message $ct::Text)) { $true }
    }
    $null = Require-AppControl 'Audit Album' $ct::ListItem
    $null = Require-AppControl 'Audit Alpha' $ct::ListItem
    Note 'columns-recovery=Title explicitly shown; group and track preserved'
}

$status = 'ok'
$app = $null
try {
    Note ('exe-sha256=' + (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash)
    Note ('probe-sha256=' + (Get-FileHash -LiteralPath $probe -Algorithm SHA256).Hash)
    $seedStart = New-Object System.Diagnostics.ProcessStartInfo
    $seedStart.FileName = $probe
    $seedStart.Arguments = '--seed-album-columns'
    $seedStart.WorkingDirectory = Split-Path $probe
    $seedStart.UseShellExecute = $false
    $seedStart.RedirectStandardOutput = $true
    $seedStart.RedirectStandardError = $true
    $seedStart.StandardOutputEncoding = [Text.Encoding]::UTF8
    $seed = [Diagnostics.Process]::Start($seedStart)
    $seedOutput = $seed.StandardOutput.ReadToEndAsync()
    $seedError = $seed.StandardError.ReadToEndAsync()
    $seed.WaitForExit()
    if ($seed.ExitCode -ne 0) { throw "Fixture seed failed: $($seedError.GetAwaiter().GetResult())" }
    $scratch = $seedOutput.GetAwaiter().GetResult().Trim()
    $expectedParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    $prefix = "aobus-uia-album-columns-$($seed.Id)-"
    if (-not [IO.Path]::IsPathRooted($scratch) -or (Split-Path -Parent $scratch) -ne $expectedParent -or
        (Split-Path -Leaf $scratch) -notmatch ('^' + [regex]::Escape($prefix) + '\d+$') -or
        -not (Test-Path -LiteralPath (Join-Path $scratch 'library') -PathType Container)) {
        throw "Fixture seed did not identify its own temporary library: $scratch"
    }
    Note "seed-owned-root=$scratch"
    $privateProfile = Join-Path $scratch 'profile'
    New-Item -ItemType Directory -Path $privateProfile | Out-Null
    Note "private-app-profile=$privateProfile"
    $library = Join-Path $scratch 'library'
    $app = Start-IsolatedApp ('--aobus-successor --library-root "' + $library + '"')
    if (-not $app -or $app.MainWindowHandle -eq [IntPtr]::Zero) { throw 'First WinUI launch failed' }
    $firstPid = $app.Id
    Note "app-process=initial pid=$firstPid"
    $null = Require-AppControl 'Audit Alpha' $ct::ListItem
    $main = Get-Main
    if ($main.Current.ProcessId -ne $app.Id) { throw 'The realized window is not owned by the test process' }
    Note "winui-main-class=$($main.Current.ClassName)"
    $presentation = Wait-Native 'presentation picker' { Get-Presentation }
    if ($presentation.Current.Name -ne 'Albums') {
        $presentation.GetCurrentPattern($invoke).Invoke()
        (Require-AppControl 'Albums' $ct::MenuItem).GetCurrentPattern($invoke).Invoke()
        Note 'initial-presentation=Albums source=native-picker'
    } else {
        Note 'initial-presentation=Albums source=unseeded-recommendation'
    }
    Assert-AlbumBody $false '01-initial-hidden'
    $untitled = Require-AppControl 'untitled-filename.flac' $ct::ListItem
    if (Find-AppControl 'File Path' $ct::Button) { throw 'Expected File Path column to be absent' }
    Note "untitled-peer=$($untitled.Current.Name) role=$($untitled.Current.ControlType.ProgrammaticName) file-path-column-hidden=True"
    $album = Open-Columns
    Assert-Toggle $album $false 'initial'
    Invoke-Toggle $album
    Assert-AlbumBody $true '02-shown'
    Select-List 'Bach'
    $bachPresentation = Wait-Native 'Bach view replacing All Tracks' {
        $p = Get-Presentation
        if ($p -and $p.Current.Name -ne 'Albums') { $p }
    }
    Note "bach-presentation=$($bachPresentation.Current.Name)"
    Select-List 'All Tracks'
    Assert-AlbumBody $true '03-returned'
    Close $app
    if ($app.ExitCode -ne 0) { throw "First process exit=$($app.ExitCode)" }
    $app = $null

    $app = Start-IsolatedApp ''
    if (-not $app -or $app.MainWindowHandle -eq [IntPtr]::Zero -or $app.Id -eq $firstPid) { throw 'Second WinUI launch failed' }
    $secondPid = $app.Id
    Note "app-process=restarted pid=$secondPid"
    Assert-AlbumBody $true '04-restarted-shown'
    $album = Open-Columns
    Assert-Toggle $album $true 'restarted-shown'
    Invoke-Toggle $album
    $null = Wait-Native 'row Album header removed after uncheck' {
        if (-not (Find-AppControl 'Album' $ct::Button)) { $true }
    }
    Assert-AlbumBody $false '05-unchecked-hidden'
    Close $app
    if ($app.ExitCode -ne 0) { throw "Second process exit=$($app.ExitCode)" }
    $app = $null

    $app = Start-IsolatedApp ''
    if (-not $app -or $app.MainWindowHandle -eq [IntPtr]::Zero -or $app.Id -in @($firstPid, $secondPid)) {
        throw 'Third WinUI launch failed'
    }
    Note "app-process=restarted-again pid=$($app.Id)"
    Assert-AlbumBody $false '06-restarted-hidden'
    $album = Open-Columns
    Assert-Toggle $album $false 'restarted-hidden'
    Assert-LastVisible $album

    Prepare-NoVisibleColumns
    $thirdPid = $app.Id
    Close $app
    if ($app.ExitCode -ne 0) { throw "Third process exit=$($app.ExitCode)" }
    $app = $null
    $layoutFile = Join-Path $library '.aobus\winui_layout.yaml'
    $savedEmptyLayout = Get-LayoutReceipt $layoutFile
    $app = Start-IsolatedApp ''
    if (-not $app -or $app.MainWindowHandle -eq [IntPtr]::Zero -or $app.Id -in @($firstPid, $secondPid, $thirdPid)) {
        throw 'Could not relaunch the saved zero-column Albums view'
    }
    $null = Require-AppControl 'No track columns are visible in this presentation.' $ct::Text
    if ((Get-LayoutReceipt $layoutFile) -ne $savedEmptyLayout) { throw 'Displaying the recovery message rewrote the saved layout' }
    Check-NoVisibleColumnsRecovery $layoutFile
} catch {
    $status = 'error'
    Note "plan=failed $($_.Exception.Message) position=$($_.InvocationInfo.PositionMessage)"
} finally {
    if (-not $app) { $app = $script:ownedProcess }
    try {
        Close $app
        if ($app -and $app.ExitCode -ne 0) { throw "Final process exit=$($app.ExitCode)" }
    } catch {
        $status = 'error'
        Note "close=failed $($_.Exception.Message)"
    }
    if ($status -ne 'ok') { exit 1 }
}
