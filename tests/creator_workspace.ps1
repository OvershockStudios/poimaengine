# SPDX-License-Identifier: Apache-2.0
# Run in the interactive Windows session against a portable desktop directory.
# UI Automation addresses only windows owned by this run. No file picker,
# physical input or pre-existing project is used. -WithCodex opens a local
# Codex connection with the installed client and submits no model turn.
# -Antigravity and -Claude use the supplied ACP clients for one MCP read each.
# -CodexBrief runs a two-turn interview on the unchanged starter project.
# -CodexQuestions uses the local AgentRpc contract executable as a scripted app-server.
# -WithExport requires a packaged sample runtime and plays a relocated export.
param(
    [Parameter(Mandatory = $true)][string]$Editor,
    [Parameter(Mandatory = $true)][string]$Output,
    [switch]$WithCodex,
    [string]$Antigravity,
    [string]$Claude,
    [switch]$CodexBrief,
    [string]$CodexQuestions,
    [switch]$WithExport
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
    codex_questions_requested = ![string]::IsNullOrWhiteSpace($CodexQuestions)
    export_requested = [bool]$WithExport
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

function Find-ControlId($Id) {
    $condition = [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
    return $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
}

function Select-QuestionChoice($Control) {
    $pattern = $null
    if ($Control.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$pattern)) {
        $pattern.Select()
    } elseif ($Control.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$pattern)) {
        $pattern.Toggle()
    } else {
        throw 'Question choice does not expose an accessible selection action.'
    }
}

function Select-AgentProvider($Name) {
    $selector = Wait-For 'the agent selector' { Find-Control 'Agent provider' -Enabled }
    $selection = $null
    if ($selector.TryGetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern, [ref]$selection) -and
        @($selection.Current.GetSelection() | Where-Object { $_.Current.Name -eq $Name }).Count -gt 0) { return }
    $selector.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    $condition = [System.Windows.Automation.AndCondition]::new(
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::NameProperty, $Name),
        [System.Windows.Automation.PropertyCondition]::new(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $owner.Id))
    $option = Wait-For "the $Name option" {
        $items = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
            [System.Windows.Automation.TreeScope]::Descendants, $condition)
        foreach ($item in $items) {
            if ($item.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$selection)) { return $item }
        }
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
        'Please follow the project game-brief interview. Ask short multiple-choice questions as numbered text in your chat reply. ' +
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

function Exercise-CodexQuestions($Project, $Peer) {
    Select-AgentProvider 'Codex'
    Set-AgentExecutable 'Codex executable' $Peer
    Invoke-Control 'Agent Connect'
    $null = Wait-For 'the scripted question connection' { Find-Control 'Agent Send' -Enabled } 65
    Set-ControlText 'Agent prompt' 'Ask me about a game.'
    Invoke-Control 'Agent Send'
    $first = Wait-For 'the first choice' { Find-ControlId 'agent-question-1-choice-1' }
    if (Find-Control 'Send answers' -Enabled) { throw 'An unanswered question card can be submitted.' }
    Select-QuestionChoice $first
    if (Find-Control 'Send answers' -Enabled) { throw 'A partially answered round can be submitted.' }
    $other = Wait-For 'a custom answer option' { Find-ControlId 'agent-question-2-other' }
    Select-QuestionChoice $other
    $text = Wait-For 'the custom answer field' { Find-ControlId 'agent-question-2-answer' }
    $text.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue("After the rain, " + [char]0x6c34)
    Invoke-Control 'Send answers'
    $null = Wait-For 'the scripted peer accepting the exact reply' {
        (Find-Control 'Agent Send' -Enabled) -and (Get-ControlText 'Agent transcript').Contains('QUESTIONS_ACCEPTED')
    }
    if (Find-ControlId 'agent-question-1-choice-1') { throw 'The answered card remained in the next turn.' }
    $current = Inspect-Project
    if ($current.world_sha256 -cne $Project.world_sha256 -or $current.manifest_sha256 -cne $Project.manifest_sha256) {
        throw 'The question exchange changed the project.'
    }
    $report.checks.codex_questions = @{
        exact_reply_verified_by_peer = $true; rejected_other_thread = $true; custom_unicode = $true
        no_automatic_answer = $true; card_retired = $true; project_unchanged = $true
    }
    Invoke-Control 'Agent Disconnect'
    $null = Wait-For 'the retired question connection' { Find-Control 'Agent Connect' -Enabled }
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

function Export-Project($Project) {
    Invoke-Control 'Export Windows build'
    $null = Wait-For 'the exported build folder control' { Find-Control 'Open build folder' -Enabled } 60
    $folders = @(Get-ChildItem -LiteralPath $projects -Directory -Filter "$name - Build *")
    if ($folders.Count -ne 1) { throw 'Expected one new standalone build beside the project.' }
    $bundle = $folders[0].FullName
    $gameManifest = Join-Path $bundle 'game.json'
    $response = & $cli game inspect $gameManifest | ConvertFrom-Json
    $inspected = $LASTEXITCODE -eq 0
    $game = Get-Content -LiteralPath $gameManifest -Raw | ConvertFrom-Json
    $world = Get-Content -LiteralPath (Join-Path $bundle $game.entry.world) -Raw | ConvertFrom-Json
    $camera = $world.entities.PSObject.Properties[$game.entry.camera].Value
    $current = Inspect-Project
    if (!$inspected -or !$response.result -or $response.result.project_id -cne $Project.project_id -or
        $response.result.revision -ne $Project.revision -or $response.result.target_os -cne 'Windows' -or
        $game.source.world_sha256 -cne $Project.world_sha256 -or $camera.name -cne 'Export viewpoint' -or
        $current.manifest_sha256 -cne $Project.manifest_sha256 -or $current.world_sha256 -cne $Project.world_sha256) {
        throw 'The exported bundle identity or saved project failed inspection.'
    }
    $report.checks.export = [ordered]@{
        project_id = $response.result.project_id
        revision = $response.result.revision
        target_os = $response.result.target_os
        manifest_sha256 = (Get-FileHash -LiteralPath $gameManifest).Hash.ToLowerInvariant()
        file_count = $response.result.files.Count
    }
    return $bundle
}

function Edit-ProjectForExport($Project) {
    $request = @{
        jsonrpc = '2.0'; id = 1; method = 'world.transact'
        params = @{
            request_id = [guid]::NewGuid().ToString('N'); base_revision = $Project.revision
            ops = @(@{ op = 'entity.rename'; id = $Project.entry.camera; name = 'Export viewpoint' })
        }
    } | ConvertTo-Json -Depth 6 -Compress
    $response = $request | & $cli connect $endpoint --timeout-ms 2000 | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or !$response.result -or $response.result.revision -ne ($Project.revision + 1)) {
        throw 'The shared owner did not commit the export test edit.'
    }
    $saved = Inspect-Project
    if ($saved.revision -ne $response.result.revision -or $saved.world_sha256 -ceq $Project.world_sha256 -or
        $saved.manifest_sha256 -cne $Project.manifest_sha256) {
        throw 'The shared edit did not produce the expected saved project revision.'
    }
    $report.checks.edit_before_export = [ordered]@{ revision = $saved.revision; world_sha256 = $saved.world_sha256 }
    return $saved
}

function Play-RelocatedExport($Bundle) {
    $source = Split-Path -Parent $manifest
    Move-Item -LiteralPath $source -Destination (Join-Path $run 'retired source')
    if (Test-Path -LiteralPath $manifest) { throw 'The original project remains available.' }
    $relocated = Join-Path $run 'Relocated Windows build'
    Move-Item -LiteralPath $Bundle -Destination $relocated
    $gameManifest = Join-Path $relocated 'game.json'
    $replay = Join-Path $run 'export-replay.json'
    $capture = Join-Path $run 'export-frame.bmp'
    $playerReport = Join-Path $run 'export-player.json'
    $launcher = Join-Path $relocated 'launch.cmd'
    $localData = Join-Path $run 'player local data'
    New-Item -ItemType Directory -Path $localData | Out-Null
    '[{"ticks":12,"move":[0,1]}]' | Set-Content -LiteralPath $replay -Encoding ASCII
    $path = $env:PATH
    $originalLocalData = $env:LOCALAPPDATA
    Push-Location $run
    try {
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        $env:LOCALAPPDATA = $localData
        & $launcher --replay $replay --width 640 --height 360 --capture $capture --report $playerReport |
            Set-Content -LiteralPath (Join-Path $run 'export-player.stdout.txt')
        $exitCode = $LASTEXITCODE
    }
    finally {
        $env:PATH = $path
        $env:LOCALAPPDATA = $originalLocalData
        Pop-Location
    }
    if ($exitCode -ne 0) { throw "The relocated player exited with $exitCode." }
    $result = Get-Content -LiteralPath $playerReport -Raw | ConvertFrom-Json
    $saveRoot = Join-Path $localData "Poima\Games\$($report.checks.export.project_id)\Saves"
    if (!$result.play.success -or !$result.play.hardware -or $result.play.tick -ne 12 -or
        $result.play.frames_presented -lt 1 -or $result.play.stop_reason -cne 'replay_complete' -or
        $result.play.nvrhi_errors -ne 0 -or !(Test-Path -LiteralPath $capture) -or !(Test-Path -LiteralPath $saveRoot -PathType Container) -or
        (Get-FileHash -LiteralPath $gameManifest).Hash.ToLowerInvariant() -cne $report.checks.export.manifest_sha256) {
        throw 'Native playback of the relocated export failed.'
    }
    $report.checks.relocated_export = [ordered]@{
        original_project_unavailable = !(Test-Path -LiteralPath $manifest)
        exit_code = $exitCode
        tick = $result.play.tick
        presented_frames = $result.play.frames_presented
        gpu = $result.play.gpu
        external_save_directory_created = $true
        capture_sha256 = (Get-FileHash -LiteralPath $capture).Hash.ToLowerInvariant()
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
    if ($WithExport) {
        if (!(Find-Control 'Export Windows build') -or (Find-Control 'Export Windows build' -Enabled)) {
            throw 'The Windows export control should be unavailable while Play is active.'
        }
        $report.checks.export_disabled_during_preview = $true
    }
    Invoke-Control 'Stop preview'
    $report.checks.stopped = [bool](Wait-For 'the stopped Game preview' {
        $state = Inspect-Desktop
        !$state.runtime.active -and !$state.views.game.attached
    })
    if ($WithExport) {
        $project = Edit-ProjectForExport $project
        $bundle = Export-Project $project
    }
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
    if ($CodexQuestions) {
        if ($WithCodex) { Invoke-Control 'Agent Disconnect' }
        Exercise-CodexQuestions $project $CodexQuestions
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
    if ($WithExport) { Play-RelocatedExport $bundle }
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
