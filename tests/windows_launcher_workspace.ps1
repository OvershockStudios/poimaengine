# SPDX-License-Identifier: Apache-2.0
# Uses the published launcher and release ZIP in an interactive Windows session.
param(
    [Parameter(Mandatory = $true)][string]$Launcher,
    [Parameter(Mandatory = $true)][string]$EngineZip,
    [Parameter(Mandatory = $true)][string]$Output,
    [switch]$WithBuildCheck
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class InstalledCreatorWindow {
    private delegate bool Callback(IntPtr window, IntPtr context);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr context);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr window, Callback callback, IntPtr context);
    [DllImport("user32.dll")] private static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll")] private static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    private static extern int GetClassName(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wparam, string text,
        uint flags, uint timeout, out IntPtr result);
    public static IntPtr Find(uint process, string title) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((window, _) => {
            uint id;
            GetWindowThreadProcessId(window, out id);
            if (id != process || !IsWindowVisible(window)) return true;
            var caption = new StringBuilder(256);
            GetWindowText(window, caption, caption.Capacity);
            if (caption.ToString() != title) return true;
            result = window;
            return false;
        }, IntPtr.Zero);
        return result;
    }
    public static bool ChooseFile(IntPtr dialog, string file) {
        IntPtr edit = IntPtr.Zero;
        EnumChildWindows(dialog, (window, _) => {
            var kind = new StringBuilder(32);
            GetClassName(window, kind, kind.Capacity);
            if (kind.ToString() != "Edit" ||
                (GetDlgCtrlID(window) != 1148 && GetDlgCtrlID(GetParent(window)) != 1148)) return true;
            edit = window;
            return false;
        }, IntPtr.Zero);
        IntPtr accepted;
        if (edit == IntPtr.Zero ||
            SendMessageTimeout(edit, 0x000c, IntPtr.Zero, file, 3, 5000, out accepted) == IntPtr.Zero ||
            accepted == IntPtr.Zero) return false;
        IntPtr open = GetDlgItem(dialog, 1);
        return open != IntPtr.Zero && PostMessage(open, 0x00f5, IntPtr.Zero, IntPtr.Zero);
    }
}
'@

$Launcher = (Resolve-Path -LiteralPath $Launcher).Path
$EngineZip = (Resolve-Path -LiteralPath $EngineZip).Path
$manifest = Get-Content -Raw -LiteralPath ([IO.Path]::ChangeExtension($EngineZip, '.manifest.json')) | ConvertFrom-Json
$version = $manifest.engine_version
$run = Join-Path ([IO.Path]::GetFullPath($Output)) ([guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $run | Out-Null
$exe = Join-Path $run 'Poima.Launcher.exe'
Copy-Item -LiteralPath $Launcher -Destination $exe
$root = Join-Path $run 'PoimaData'
$projects = Join-Path $root 'Projects'
$installed = Join-Path $root "Engines\versions\Poima-$version-Windows-x64"
$editorPath = Join-Path $installed 'Poima.Editor.exe'
$ordinary = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Poima\installer.json'
$extraction = $env:DOTNET_BUNDLE_EXTRACT_BASE_DIR
$env:DOTNET_BUNDLE_EXTRACT_BASE_DIR = Join-Path $run 'launcher-bundle'
$oldTemp, $oldTmp, $oldPackages, $oldPath = $env:TEMP, $env:TMP, $env:NUGET_PACKAGES, $env:PATH
$testTemp = Join-Path $run 'temp'
$testPackages = Join-Path $run 'packages'
if ($WithBuildCheck) {
    $null = New-Item -ItemType Directory -Path $testTemp, $testPackages
    $env:TEMP = $env:TMP = $testTemp
    $env:NUGET_PACKAGES = $testPackages
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
}
$owner = $creator = $null
$report = [ordered]@{ passed = $false; engine_version = $version; checks = [ordered]@{} }

function Hash($Path) { (Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() }
function SettingsHash {
    if (Test-Path -LiteralPath $ordinary) { Hash $ordinary }
}
function Wait-For($Label, [scriptblock]$Probe, [int]$Seconds = 35) {
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while ($deadline.Elapsed.TotalSeconds -lt $Seconds) {
        $found = & $Probe
        if ($found) { return $found }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Label."
}
function Window($Process, $Title) {
    if ($Process.HasExited) { throw "$Title process exited before its window appeared." }
    $hwnd = [InstalledCreatorWindow]::Find($Process.Id, $Title)
    if ($hwnd -ne [IntPtr]::Zero) { [Windows.Automation.AutomationElement]::FromHandle($hwnd) }
}
function Element($Window, $Name) {
    $match = [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::NameProperty, $Name)
    $item = $Window.FindFirst([Windows.Automation.TreeScope]::Descendants, $match)
    if ($item -and $item.Current.IsEnabled) { $item }
}
function Invoke-Button($Window, $Name) {
    $button = Wait-For $Name { Element $Window $Name }
    $button.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke()
}
function Set-Field($Field, $Text) {
    $Field.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern).SetValue($Text)
}
function Close-ProcessWindow($Process) {
    if (!$Process.CloseMainWindow() -or !$Process.WaitForExit(20000) -or $Process.ExitCode -ne 0) {
        throw "Process $($Process.Id) did not close normally."
    }
}

function Check-GameplayBuild($HomeWindow) {
    Invoke-Button $HomeWindow 'Set up gameplay builds'
    $setup = Wait-For 'gameplay setup' { Window $creator 'Set up gameplay builds' }
    $id = [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::AutomationIdProperty,
        'gameplay-build-status')
    $status = Wait-For 'gameplay status' { $setup.FindFirst([Windows.Automation.TreeScope]::Descendants, $id) }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    Invoke-Button $setup 'Test gameplay build'
    $null = Wait-For 'starter build in progress' { Element $setup 'Cancel test' }
    $success = 'Build passed. The compiled starter ran and responded to input.'
    $null = Wait-For 'first starter build' {
        if (Element $setup 'Test gameplay build') {
            if ($status.Current.Name -cne $success) { throw "Build check failed: $($status.Current.Name)" }
            $true
        }
    } 300
    $report.checks.gameplay_build_check_first_ms = $timer.ElapsedMilliseconds
    $report.checks.fresh_nuget_packages = @(Get-ChildItem -LiteralPath $testPackages -Directory).Count
    if ($report.checks.fresh_nuget_packages -eq 0) { throw 'The first check did not populate its empty package directory.' }

    Invoke-Button $setup 'Test gameplay build'
    $publisher = Wait-For 'owned native publisher' {
        Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
            Where-Object { $_.CommandLine -like "*$testTemp\poima-build-check-*" -and $_.CommandLine -like '*publish_native_gameplay.py*' } |
            Select-Object -First 1
    }
    Invoke-Button $setup 'Cancel test'
    $null = Wait-For 'cancelled build' {
        if (Element $setup 'Test gameplay build') {
            if ($status.Current.Name -cne 'Build test cancelled.') { throw "Build cancel failed: $($status.Current.Name)" }
            $true
        }
    } 300
    if (Get-Process -Id $publisher.ProcessId -ErrorAction SilentlyContinue) { throw 'The cancelled publisher is still running.' }
    $report.checks.active_build_cancelled = $true

    Invoke-Button $setup 'Test gameplay build'
    $null = Wait-For 'starter retry in progress' { Element $setup 'Cancel test' }
    $null = Wait-For 'starter build retry' {
        if (Element $setup 'Test gameplay build') {
            if ($status.Current.Name -cne $success) { throw "Build retry failed: $($status.Current.Name)" }
            $true
        }
    } 300
    if (Get-ChildItem -LiteralPath $testTemp -Directory -Filter 'poima-build-check-*') { throw 'Build checks left a test project behind.' }
    if ((Test-Path -LiteralPath $projects) -and (Get-ChildItem -LiteralPath $projects -Force)) {
        throw 'Build checks wrote to the project library.'
    }
    $report.checks.gameplay_retry_passed = $true
    $report.checks.gameplay_scratch_removed = $true
    Invoke-Button $setup 'Done'
}

$before = SettingsHash
try {
    if (Test-Path -LiteralPath $root) { throw 'The portable install root must be empty.' }
    if ((Hash $EngineZip) -cne $manifest.archive.sha256) { throw 'The supplied engine ZIP does not match the inventory.' }
    $report.archive_sha256 = $manifest.archive.sha256
    $report.launcher_sha256 = Hash $exe
    $owner = Start-Process -FilePath $exe -ArgumentList '--portable' -WorkingDirectory $run -PassThru
    $null = $owner.Handle
    $window = Wait-For 'portable launcher' { Window $owner 'Poima Launcher' }
    if (Element $window 'Open Poima') { throw 'An empty profile offered to open an existing engine.' }
    $installLabel = 'Install from ZIP' + [char]0x2026
    $pick = Wait-For $installLabel { Element $window $installLabel }
    $buttonHandle = [IntPtr]$pick.Current.NativeWindowHandle
    # The WinForms button opens a synchronous modal dialog on its UI thread.
    if ($buttonHandle -eq [IntPtr]::Zero -or
        ![InstalledCreatorWindow]::PostMessage($buttonHandle, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)) {
        throw 'Could not open the engine ZIP chooser.'
    }
    $dialog = Wait-For 'engine ZIP picker' { Window $owner 'Select a Poima engine ZIP' }
    if (![InstalledCreatorWindow]::ChooseFile([IntPtr]$dialog.Current.NativeWindowHandle, $EngineZip)) {
        throw 'Could not select the engine ZIP in the owned dialog.'
    }
    $null = Wait-For 'installed engine' { (Test-Path -LiteralPath $editorPath) -and (Element $window 'Open Poima') } 120
    foreach ($entry in $manifest.files) {
        if ((Hash (Join-Path $installed $entry.path)) -cne $entry.sha256) {
            throw "Installed file differs from the release inventory: $($entry.path)"
        }
    }
    $report.checks.installed_inventory_files = $manifest.files.Count
    $report.checks.install_staging_empty = !(Get-ChildItem -LiteralPath (Join-Path $root 'Engines\.poima-staging') -Force)
    if (!$report.checks.install_staging_empty) { throw 'Installation left staging files.' }
    Invoke-Button $window 'Open Poima'
    $creator = Wait-For 'installed creator process' {
        Get-Process -Name Poima.Editor -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -ieq $editorPath } | Select-Object -First 1
    }
    $null = $creator.Handle
    $creatorHomeWindow = Wait-For 'installed creator home' { Window $creator 'Poima' }
    if ($WithBuildCheck) { Check-GameplayBuild $creatorHomeWindow }
    $field = Wait-For 'new project name' { Element $creatorHomeWindow 'Project name' }
    Set-Field $field 'Portable Starter'
    Invoke-Button $creatorHomeWindow 'Create project'
    $projectPath = Join-Path $projects 'Portable Starter\project.poimaproject'
    $null = Wait-For 'new portable project' { Test-Path -LiteralPath $projectPath -PathType Leaf }
    $workspace = Wait-For 'installed workspace' {
        $creator.Refresh()
        $hwnd = $creator.MainWindowHandle
        if ($hwnd -eq [IntPtr]::Zero) { return }
        $candidate = [Windows.Automation.AutomationElement]::FromHandle($hwnd)
        if ($candidate.Current.Name -like 'Portable Starter*Poima Creator') { $candidate }
    }
    $null = Wait-For 'installed workspace play control' { Element $workspace 'Play project' }
    $report.checks.portable_project_created = $true
    $report.checks.creator_editor_sha256 = Hash $editorPath
    Close-ProcessWindow $creator
    Close-ProcessWindow $owner
    if ((SettingsHash) -cne $before) { throw 'Portable mode modified normal launcher settings.' }
    $report.checks.default_launcher_settings_preserved = $true
    $report.checks.launcher_exit = $owner.ExitCode
    $report.checks.creator_exit = $creator.ExitCode
    $report.passed = $true
}
catch {
    $report.error = $_.ToString()
    throw
}
finally {
    foreach ($process in @($creator, $owner)) {
        if ($process -and !$process.HasExited) {
            $report.forced_cleanup = $true
            $process.Kill()
            $process.WaitForExit()
        }
    }
    $env:DOTNET_BUNDLE_EXTRACT_BASE_DIR = $extraction
    $env:TEMP, $env:TMP, $env:NUGET_PACKAGES, $env:PATH = $oldTemp, $oldTmp, $oldPackages, $oldPath
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'report.json') -Encoding UTF8
    Write-Output $run
}
