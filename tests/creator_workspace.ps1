# SPDX-License-Identifier: Apache-2.0
# Run in the interactive Windows session against a portable desktop directory.
# UI Automation addresses only windows owned by this run. No file picker,
# physical input or pre-existing project is used. -WithCodex opens a local
# Codex connection with the installed client and submits no model turn.
# -Antigravity and -Claude use the supplied ACP clients for one MCP read each.
# -CodexBrief runs a two-turn interview on the unchanged starter project.
param(
    [Parameter(Mandatory = $true)][string]$Editor,
    [Parameter(Mandatory = $true)][string]$Output,
    [switch]$WithCodex,
    [string]$Antigravity,
    [string]$Claude,
    [switch]$CodexBrief
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CreatorTestWindow {
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
}
'@

$exe = (Resolve-Path -LiteralPath $Editor).Path
$package = Split-Path -Parent $exe
$cli = Join-Path $package 'poima.exe'
$run = Join-Path ([System.IO.Path]::GetFullPath($Output)) ([guid]::NewGuid().ToString('N'))
$projects = Join-Path $run 'projects'
$name = 'Creator Workspace'
$manifest = Join-Path (Join-Path $projects $name) 'project.poimaproject'
New-Item -ItemType Directory -Path $projects | Out-Null
$owner = $null
$window = $null
$report = [ordered]@{
    passed = $false
    input = 'Windows UI Automation and an owned WM_CLOSE message'
    editor_sha256 = (Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()
    frontend_sha256 = (Get-FileHash -LiteralPath (Join-Path $package 'Poima.Editor.dll')).Hash.ToLowerInvariant()
    cli_sha256 = (Get-FileHash -LiteralPath $cli).Hash.ToLowerInvariant()
    bridge_sha256 = (Get-FileHash -LiteralPath (Join-Path $package 'poima_desktop.dll')).Hash.ToLowerInvariant()
    codex_connection_requested = [bool]$WithCodex
    antigravity_turn_requested = ![string]::IsNullOrEmpty($Antigravity)
    claude_turn_requested = ![string]::IsNullOrEmpty($Claude)
    codex_brief_requested = [bool]$CodexBrief
    checks = [ordered]@{}
}

function Wait-For($Label, [scriptblock]$Probe, [int]$Timeout = 30) {
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt $Timeout) {
        if ($owner.HasExited) { throw "Poima exited while waiting for $Label." }
        $result = & $Probe
        if ($result) { return $result }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Label."
}

function Find-Window($Title) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $owner.Id)
    $windows = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
        [System.Windows.Automation.TreeScope]::Children, $condition)
    foreach ($candidate in $windows) {
        if ($candidate.Current.Name -like $Title) { return $candidate }
    }
}

function Find-Control($Label, [switch]$Enabled) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::NameProperty, $Label)
    $control = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
    if ($control -and (!$Enabled -or $control.Current.IsEnabled)) { return $control }
}

function Invoke-Control($Label) {
    $control = Wait-For $Label { Find-Control $Label -Enabled }
    $control.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}

function Get-ControlText($Label) {
    $control = Wait-For $Label { Find-Control $Label }
    return $control.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value
}

function Set-ControlText($Label, $Value) {
    $control = Wait-For $Label { Find-Control $Label -Enabled }
    $control.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($Value)
}

function Select-AgentProvider($Name) {
    $selector = Wait-For 'the agent selector' { Find-Control 'Agent provider' -Enabled }
    $selector.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    $condition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::NameProperty, $Name),
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $owner.Id))
    $option = Wait-For "the $Name option" {
        [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Descendants, $condition)
    }
    $option.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
}

function Set-AgentExecutable($Control, $Path) {
    $settings = Wait-For 'Connection settings' { Find-Control 'Connection settings' -Enabled }
    $settings.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    Set-ControlText $Control (Resolve-Path -LiteralPath $Path).Path
    $settings.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse()
}

function Send-CodexPrompt($Prompt) {
    Set-ControlText 'Agent prompt' $Prompt
    Invoke-Control 'Agent Send'
    $prefix = "You: $Prompt"
    $null = Wait-For 'the submitted Codex prompt' {
        (Get-ControlText 'Agent transcript').Contains($prefix)
    }
    $null = Wait-For 'the completed Codex response' {
        $transcript = Get-ControlText 'Agent transcript'
        $transcript | Set-Content -LiteralPath (Join-Path $run 'codex-transcript.txt') -Encoding UTF8
        $reply = $transcript.Substring($transcript.LastIndexOf($prefix) + $prefix.Length).Trim()
        if ((Find-Control 'Agent Send' -Enabled) -and $reply.StartsWith('Codex:') -and $reply.Length -gt 6) {
            return $true
        }
    } 120
    return Get-ControlText 'Agent transcript'
}

function Exercise-CodexBrief($Project) {
    Select-AgentProvider 'Codex'
    Invoke-Control 'Agent Connect'
    $null = Wait-For 'the connected Codex conversation' { Find-Control 'Agent Send' -Enabled } 65
    $idea = ('I want to make my first game. A quiet cave where I collect three glowing crystals and open an exit. ' +
        'Please follow the project game-brief interview. Ask short multiple-choice questions in this chat. ' +
        'Leave the project untouched until I explicitly approve the brief.')
    $interview = Send-CodexPrompt $idea
    $interview | Set-Content -LiteralPath (Join-Path $run 'codex-interview.txt') -Encoding UTF8
    $answer = ('First person, no timer, and keyboard controls using WASD to move and E to interact. ' +
        'Use your judgment for the cave layout, colors, and exit placement. ' +
        'Please summarize the brief and the first playable acceptance check. Wait for my approval.')
    $brief = Send-CodexPrompt $answer
    $brief | Set-Content -LiteralPath (Join-Path $run 'codex-brief.txt') -Encoding UTF8
    $current = Inspect-Project
    if ($current.world_sha256 -ne $Project.world_sha256 -or $current.manifest_sha256 -ne $Project.manifest_sha256) {
        throw 'The Codex interview changed the project before brief approval.'
    }
    $report.checks.codex_brief = [ordered]@{
        responded_to_both_prompts = $true
        project_files_unchanged = $true
    }
    Invoke-Control 'Agent Disconnect'
    $null = Wait-For 'the retired Codex connection' { Find-Control 'Agent Connect' -Enabled }
}

function Exercise-Acp($Project, $Provider, $Setting, $Executable, $Key) {
    $world = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $manifest) $Project.entry.world) -Raw | ConvertFrom-Json
    Select-AgentProvider $Provider
    Set-AgentExecutable $Setting $Executable
    Invoke-Control 'Agent Connect'
    $null = Wait-For "the connected $Provider conversation" { Find-Control 'Agent Send' -Enabled } 65
    $report.checks["${Key}_ready"] = $true
    $prompt = ('Use the Poima MCP tools to call only world.inspect and read the world ID and revision. ' +
        'Do not write files, edit the world, run commands, or use other tools. ' +
        'Reply with one line containing POIMA_WORLD=<world_id>/<revision> using the values you read, with no markdown.')
    Set-ControlText 'Agent prompt' $prompt
    $priorTranscript = Get-ControlText 'Agent transcript'
    Invoke-Control 'Agent Send'
    $null = Wait-For "the submitted $Provider prompt" {
        (Find-Control 'Agent Stop' -Enabled) -or (Get-ControlText 'Agent transcript') -cne $priorTranscript
    }
    $report.checks["${Key}_permissions"] = @()
    $null = Wait-For "the completed $Provider MCP read" {
        $card = Find-Control 'Approve once' -Enabled
        if ($card) {
            $details = Get-ControlText 'Agent approval details'
            $details | Add-Content -LiteralPath (Join-Path $run "$Key-permission-details.txt")
            if ($details -notmatch 'world\.inspect' -or $details -notmatch 'poima[_/ :.-]+call') {
                Invoke-Control 'Decline'
                throw "$Provider requested a tool outside the expected test operation."
            }
            $report.checks["${Key}_permissions"] += 'world.inspect'
            $card.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
        }
        $transcript = Get-ControlText 'Agent transcript'
        $transcript | Set-Content -LiteralPath (Join-Path $run "$Key-transcript.txt") -Encoding UTF8
        if (Find-Control 'Agent Send' -Enabled) {
            $reply = $transcript.Substring($transcript.LastIndexOf($prompt) + $prompt.Length)
            $pattern = '(?m)^' + [regex]::Escape($Provider) + ':\s*POIMA_WORLD=([0-9a-f]+)/([0-9]+)\s*$'
            $match = [regex]::Match($reply, $pattern)
            if (!$match.Success -or $match.Groups[1].Value -cne $world.world_id -or
                [long]$match.Groups[2].Value -ne $Project.revision) {
                throw "$Provider returned the wrong world identity or revision."
            }
            return $true
        }
    } 120
    $report.checks["${Key}_read"] = [ordered]@{ world_id = $world.world_id; revision = $Project.revision }
    $current = Inspect-Project
    if ($current.world_sha256 -ne $Project.world_sha256 -or $current.manifest_sha256 -ne $Project.manifest_sha256) {
        throw 'The read-only ACP test changed the project.'
    }
    Invoke-Control 'Agent Disconnect'
    $null = Wait-For "the retired $Provider connection" { Find-Control 'Agent Connect' -Enabled }
    $report.checks["${Key}_disconnected"] = $true
}

function Start-Launcher($Phase) {
    $process = Start-Process -FilePath $exe -ArgumentList "--projects `"$projects`"" -WorkingDirectory $package -PassThru `
        -RedirectStandardOutput (Join-Path $run "$Phase.stdout.txt") `
        -RedirectStandardError (Join-Path $run "$Phase.stderr.txt")
    $null = $process.Handle
    return $process
}

function Close-Owner {
    $handle = [IntPtr]$window.Current.NativeWindowHandle
    if (![CreatorTestWindow]::PostMessage($handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)) {
        throw 'Could not close the test window.'
    }
    if (!$owner.WaitForExit(20000)) { throw 'The creator owner failed to retire.' }
    if ($owner.ExitCode -ne 0) { throw "The creator owner exited with $($owner.ExitCode)." }
}

function Inspect-Project {
    $response = & $cli project inspect $manifest | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or !$response.result) { throw 'The project could not be inspected.' }
    return $response.result
}

function Inspect-Desktop {
    $request = '{"jsonrpc":"2.0","id":1,"method":"desktop.inspect"}'
    $reply = $request | & $cli connect $endpoint --timeout-ms 2000 | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or !$reply.result) { throw 'The shared owner could not be inspected.' }
    return $reply.result
}

function Play-Preview($Revision) {
    Invoke-Control 'Play project'
    $state = Wait-For 'a presented Game frame' {
        $candidate = Inspect-Desktop
        if ($candidate.runtime.active -and $candidate.views.game.presented_tick -gt 0) { $candidate }
    }
    $view = $state.views.game
    if ($state.runtime.authored_revision -ne $Revision -or !$view.attached -or $state.views.scene.attached -or
        $view.frames_presented -lt 1 -or !$view.render.success -or !$view.render.hardware -or $view.render.nvrhi_errors -ne 0) {
        throw 'The reported Game frame failed the native preview checks.'
    }
    return [ordered]@{
        session = $state.runtime.session_id
        tick = $view.presented_tick
        frames = $view.frames_presented
        revision = $state.runtime.authored_revision
        gpu = $view.render.gpu
        validation_errors = $view.render.nvrhi_errors
    }
}

try {
    $owner = Start-Launcher 'create'
    $window = Wait-For 'the launcher window' { Find-Window 'Poima' }
    $field = Wait-For 'Project name' { Find-Control 'Project name' -Enabled }
    $field.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($name)
    Invoke-Control 'Create project'
    $window = Wait-For 'the creator window' { Find-Window "$name*Poima Creator" }
    $project = Inspect-Project
    $endpoint = 'poima-' + $project.project_id
    $report.checks.created = [ordered]@{ id = $project.project_id; revision = $project.revision }

    $report.checks.first_preview = Play-Preview $project.revision
    Invoke-Control 'Stop preview'
    $report.checks.stopped = [bool](Wait-For 'the stopped Game preview' {
        $state = Inspect-Desktop
        !$state.runtime.active -and !$state.views.game.attached
    })
    if ($Antigravity) { Exercise-Acp $project 'Antigravity' 'Antigravity CLI executable' $Antigravity 'antigravity' }
    if ($Claude) { Exercise-Acp $project 'Claude Code' 'Claude ACP executable' $Claude 'claude' }
    if ($CodexBrief) { Exercise-CodexBrief $project }
    if ($WithCodex) {
        Select-AgentProvider 'Codex'
        Invoke-Control 'Agent Connect'
        $null = Wait-For 'the connected Codex conversation' { Find-Control 'Agent Send' -Enabled } 65
        $report.checks.codex_ready = $true
        Invoke-Control 'Agent Disconnect'
        $null = Wait-For 'the retired Codex connection' { Find-Control 'Agent Connect' -Enabled }
        $report.checks.codex_disconnected = $true
        Invoke-Control 'Agent Connect'
        $null = Wait-For 'the reconnected Codex conversation' { Find-Control 'Agent Send' -Enabled } 65
        $report.checks.codex_reconnected = $true
    }
    Close-Owner
    $report.checks.closed_exit_code = $owner.ExitCode

    $owner = Start-Launcher 'reopen'
    $window = Wait-For 'the reopened launcher' { Find-Window 'Poima' }
    Invoke-Control 'Open selected project'
    $window = Wait-For 'the reopened creator' { Find-Window "$name*Poima Creator" }
    $reopened = Inspect-Project
    if ($reopened.project_id -ne $project.project_id -or $reopened.revision -ne $project.revision) {
        throw 'Project identity or authoring revision changed after reopen.'
    }
    $report.checks.reopened_preview = Play-Preview $reopened.revision
    if ($report.checks.reopened_preview.session -eq $report.checks.first_preview.session) {
        throw 'Reopening reused the retired play session.'
    }
    Close-Owner
    $report.checks.closed_during_play_exit_code = $owner.ExitCode
    $report.passed = $true
}
catch {
    $report.error = $_.ToString()
    throw
}
finally {
    if ($owner -and !$owner.HasExited) {
        $report.forced_cleanup = $true
        Stop-Process -Id $owner.Id
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'report.json') -Encoding UTF8
    Write-Output $run
}
