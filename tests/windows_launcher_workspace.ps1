# SPDX-License-Identifier: Apache-2.0
# Uses the published launcher and release ZIP in an interactive Windows session.
# WithFirstPlay requires a working hardware Vulkan preview.
param(
    [Parameter(Mandatory = $true)][string]$Launcher,
    [Parameter(Mandatory = $true)][string]$EngineZip,
    [Parameter(Mandatory = $true)][string]$Output,
    [switch]$WithBuildCheck,
    [switch]$ExpectBuildSetup,
    [switch]$ExpectCompactStart,
    [switch]$WithFirstPlay,
    [switch]$WithSplashSettings
)

$ErrorActionPreference = 'Stop'
if ($WithBuildCheck -and $ExpectBuildSetup) { throw 'Choose the installed or unavailable build-tools path.' }
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

function Hash($Path) {
    # The engine installs with long-path support. Windows PowerShell's file
    # provider can hide valid inventory entries beyond the legacy path limit.
    $full = [IO.Path]::GetFullPath($Path)
    $extended = if ($full.StartsWith('\\?\')) { $full }
        elseif ($full.StartsWith('\\')) { '\\?\UNC\' + $full.Substring(2) }
        else { '\\?\' + $full }
    $digest = [Security.Cryptography.SHA256]::Create()
    try {
        $inputFile = [IO.File]::OpenRead($extended)
        try { [BitConverter]::ToString($digest.ComputeHash($inputFile)).Replace('-', '').ToLowerInvariant() }
        finally { $inputFile.Dispose() }
    }
    finally { $digest.Dispose() }
}
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
function Visible-Element($Window, $Name) {
    $match = [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::NameProperty, $Name)
    $items = $Window.FindAll([Windows.Automation.TreeScope]::Descendants, $match)
    foreach ($item in $items) {
        if (!$item.Current.IsOffscreen) { return $item }
    }
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

function Inspect-InstalledDesktop($Cli, $Endpoint) {
    $request = '{"jsonrpc":"2.0","id":1,"method":"desktop.inspect"}'
    $response = $request | & $Cli connect $Endpoint --timeout-ms 2000
    $exitCode = $LASTEXITCODE
    $response | Add-Content -LiteralPath (Join-Path $run 'first-play-desktop.jsonl') -Encoding UTF8
    $reply = $response | ConvertFrom-Json
    if ($exitCode -ne 0 -or $reply.id -ne 1 -or !$reply.result) {
        throw 'The installed creator owner could not be inspected.'
    }
    return $reply.result
}

function Check-FirstPlay($Workspace, $ProjectPath) {
    $cli = Join-Path $installed 'poima.exe'
    $response = & $cli project inspect $ProjectPath
    $exitCode = $LASTEXITCODE
    $response | Set-Content -LiteralPath (Join-Path $run 'first-play-project.json') -Encoding UTF8
    $receipt = $response | ConvertFrom-Json
    if ($exitCode -ne 0 -or !$receipt.result) { throw 'The installed project could not be inspected.' }
    $project = $receipt.result
    $endpoint = 'poima-' + $project.project_id
    $beforePlay = Inspect-InstalledDesktop $cli $endpoint
    if ($beforePlay.runtime.active -ne $false -or $beforePlay.views.game.attached -ne $false) {
        throw 'The installed creator already had an active preview.'
    }

    Invoke-Button $Workspace 'Play project'
    $playing = Wait-For 'a presented frame in the installed Game preview' {
        $candidate = Inspect-InstalledDesktop $cli $endpoint
        if ($candidate.runtime.active -and $candidate.views.game.presented_tick -gt 0) { $candidate }
    }
    $view = $playing.views.game
    if ($playing.runtime.authored_revision -ne $project.revision -or !$playing.runtime.session_id -or
        !$view.attached -or $playing.views.scene.attached -ne $false -or $view.frames_presented -lt 1 -or
        !$view.render.success -or !$view.render.hardware -or $view.render.nvrhi_errors -ne 0) {
        throw 'The installed Game frame failed the native preview checks.'
    }
    Invoke-Button $Workspace 'Stop preview'
    $null = Wait-For 'the installed Game preview to stop' {
        $candidate = Inspect-InstalledDesktop $cli $endpoint
        $candidate.runtime.active -eq $false -and $candidate.views.game.attached -eq $false -and
            (Element $Workspace 'Play project')
    }
    $report.checks.first_play = [ordered]@{
        project_id = $project.project_id; revision = $project.revision; session = $playing.runtime.session_id
        tick = $view.presented_tick; frames = $view.frames_presented; gpu = $view.render.gpu
        validation_errors = $view.render.nvrhi_errors; stopped = $true
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

function Check-SplashSettings($Workspace, $ProjectPath) {
    $cli = Join-Path $installed 'poima.exe'
    $original = (& $cli project inspect $ProjectPath | ConvertFrom-Json).result
    if (!$original.startup_splash) { throw 'A fresh project did not enable the splash by default.' }
    Invoke-Button $Workspace 'Project settings'
    $settings = Wait-For 'project settings window' { Window $creator 'Project settings' }
    $control = Wait-For 'splash toggle' { Element $settings 'Show the Poima splash in exported games' }
    $toggle = $control.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern)
    if ($toggle.Current.ToggleState -ne [Windows.Automation.ToggleState]::On) { throw 'The default splash toggle was off.' }
    $toggle.Toggle()
    Invoke-Button $settings 'Save'
    $changed = Wait-For 'saved splash choice' {
        $current = (& $cli project inspect $ProjectPath | ConvertFrom-Json).result
        if ($current.startup_splash -eq $false) { $current }
    }
    if ($changed.project_id -cne $original.project_id -or $changed.world_sha256 -cne $original.world_sha256 -or
        $changed.manifest_sha256 -ceq $original.manifest_sha256) { throw 'The splash edit touched the wrong project boundary.' }
    Invoke-Button $Workspace 'Project settings'
    $settings = Wait-For 'reopened project settings' { Window $creator 'Project settings' }
    $control = Wait-For 'reopened splash toggle' { Element $settings 'Show the Poima splash in exported games' }
    $toggle = $control.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern)
    if ($toggle.Current.ToggleState -ne [Windows.Automation.ToggleState]::Off) { throw 'The splash toggle did not persist.' }
    $toggle.Toggle()
    Invoke-Button $settings 'Cancel'
    $null = Wait-For 'dismissed project settings' { !(Window $creator 'Project settings') }
    $afterCancel = (& $cli project inspect $ProjectPath | ConvertFrom-Json).result
    if ($afterCancel.manifest_sha256 -cne $changed.manifest_sha256) { throw 'Cancel changed the saved manifest.' }
    $report.checks.splash_settings = [ordered]@{
        default_enabled = $true; persisted_disabled = $true; cancel_preserved_manifest = $true
    }
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
    if ($ExpectCompactStart) {
        $null = Wait-For 'Install Poima' { Element $window 'Install Poima' }
        $optionLabels = @('Install location', 'Release channel', ('Browse' + [char]0x2026), $installLabel)
        foreach ($label in $optionLabels) {
            if (Visible-Element $window $label) { throw "'$label' was visible before opening Installation options." }
        }
        $report.checks.compact_first_run = $true
        Invoke-Button $window 'Installation options'
        foreach ($optionName in $optionLabels) {
            $null = Wait-For "the expanded $optionName control" { Visible-Element $window $optionName }
        }
        $report.checks.installation_options_opened = $true
    } elseif (Element $window 'Installation options') {
        Invoke-Button $window 'Installation options'
    }
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
    if ($ExpectCompactStart) {
        $null = Wait-For 'Check for updates' { Element $window 'Check for updates' }
        $projectLabel = 'Open project' + [char]0x2026
        $null = Wait-For $projectLabel { Element $window $projectLabel }
        foreach ($label in $optionLabels) {
            if (Visible-Element $window $label) { throw "'$label' remained visible after installation." }
        }
        $report.checks.compact_installed = $true
    }
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
    $projectPath = Join-Path $projects 'Portable Starter\project.poimaproject'
    Invoke-Button $creatorHomeWindow 'Create project'
    if ($ExpectBuildSetup) {
        $setup = Wait-For 'setup before project creation' { Window $creator 'Set up gameplay builds' }
        $statusId = [Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::AutomationIdProperty,
            'gameplay-build-status')
        $message = Wait-For 'build setup result' { $setup.FindFirst([Windows.Automation.TreeScope]::Descendants, $statusId) }
        $report.checks.build_probe_reported_error = $message.Current.Name -like '*check the installed build tools*'
        if (Test-Path -LiteralPath (Split-Path -Parent $projectPath)) { throw 'Setup began after the project was created.' }
        Invoke-Button $setup 'Back'
        $null = Wait-For 'return to project form' { Element $creatorHomeWindow 'Create project' }
        if ($field.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern).Current.Value -cne 'Portable Starter' -or
            (Test-Path -LiteralPath (Split-Path -Parent $projectPath))) {
            throw 'Returning from setup changed the draft or created its folder.'
        }
        $report.checks.setup_back_preserved_draft = $true
        Invoke-Button $creatorHomeWindow 'Create project'
        $setup = Wait-For 'reopen setup from project form' { Window $creator 'Set up gameplay builds' }
        Invoke-Button $setup 'Create without build tools'
    }
    $null = Wait-For 'new portable project' { Test-Path -LiteralPath $projectPath -PathType Leaf }
    $workspace = Wait-For 'installed workspace' {
        $creator.Refresh()
        $hwnd = $creator.MainWindowHandle
        if ($hwnd -eq [IntPtr]::Zero) { return }
        $candidate = [Windows.Automation.AutomationElement]::FromHandle($hwnd)
        if ($candidate.Current.Name -like 'Portable Starter*Poima Creator') { $candidate }
    }
    $null = Wait-For 'installed workspace play control' { Element $workspace 'Play project' }
    if ($WithBuildCheck -or $ExpectBuildSetup) {
        $project = Get-Content -Raw -LiteralPath $projectPath | ConvertFrom-Json
        $profiles = Join-Path $projects ".poima-build\$($project.project_id)\profiles.json"
        if ($WithBuildCheck) {
            $profile = (Get-Content -Raw -LiteralPath $profiles | ConvertFrom-Json).profiles[0]
            if ($profile.name -cne 'creator' -or $profile.project_root -cne (Split-Path -Parent $projectPath)) {
                throw 'The new project received the wrong build profile.'
            }
            $report.checks.project_build_profile_configured = $true
        }
        else {
            if (Test-Path -LiteralPath $profiles) { throw 'Continuing without tools created a build profile.' }
            $null = Wait-For 'setup available in the created project' { Element $workspace 'Set up gameplay builds' }
            $report.checks.project_created_without_tools = $true
        }
    }
    $report.checks.portable_project_created = $true
    $report.checks.creator_editor_sha256 = Hash $editorPath
    if ($WithFirstPlay) { Check-FirstPlay $workspace $projectPath }
    if ($WithSplashSettings) { Check-SplashSettings $workspace $projectPath }
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
