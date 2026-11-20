# SPDX-License-Identifier: Apache-2.0
# Run in the interactive Windows session against a portable desktop directory.
# UI Automation addresses only windows owned by this run. No file picker,
# physical input or pre-existing project is used. -WithCodex opens a local
# Codex connection with the installed client and submits no model turn.
# -CodexExecutable selects an exact client for that installed-client check.
# -Antigravity and -Claude use the supplied ACP clients for one MCP read each.
# -CodexBrief runs a two-turn interview on the unchanged starter project.
# -CodexQuestions uses the local AgentRpc contract executable as a scripted app-server.
# -CodexToolApproval exercises decline and approve once with the installed Codex.
# -WithExport requires a packaged sample runtime and plays a relocated export.
# -WithPlaytestResults checks passing and failed reports, reopening and recovery.
# -WithShareZip also exercises the owned save dialog and plays the unpacked ZIP.
# -WithBuildHistory creates two passing versions and restores the older one through the history dialog.
# -WithoutVulkan masks drivers for the owned processes and checks preview failure recovery.
param(
    [Parameter(Mandatory = $true)][string]$Editor,
    [Parameter(Mandatory = $true)][string]$Output,
    [switch]$WithCodex,
    [string]$CodexExecutable,
    [string]$Antigravity,
    [string]$Claude,
    [switch]$CodexBrief,
    [string]$CodexQuestions,
    [switch]$CodexToolApproval,
    [switch]$WithExport,
    [switch]$WithPlaytestResults,
    [switch]$WithShareZip,
    [switch]$WithBuildHistory,
    [switch]$WithoutVulkan
)

$ErrorActionPreference = 'Stop'
if ($WithoutVulkan -and ($WithExport -or $WithPlaytestResults -or $WithShareZip -or $WithBuildHistory)) {
    throw 'Run the no-driver check separately from export and hardware playback checks.'
}
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CreatorTestWindow {
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    private delegate bool WindowCallback(IntPtr window, IntPtr context);
    [DllImport("user32.dll")] private static extern bool EnumWindows(WindowCallback callback, IntPtr context);
    [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, System.Text.StringBuilder text, int capacity);
    public static IntPtr FindDialog(IntPtr parent, string title) {
        IntPtr match = IntPtr.Zero;
        EnumWindows((window, _) => {
            if (GetWindow(window, 4) != parent || !IsWindowVisible(window)) return true;
            var text = new System.Text.StringBuilder(256);
            GetWindowText(window, text, text.Capacity);
            if (text.ToString() != title) return true;
            match = window;
            return false;
        }, IntPtr.Zero);
        return match;
    }
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
$verifiedPlayer = $null
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
    playtest_results_requested = [bool]$WithPlaytestResults
    share_zip_requested = [bool]$WithShareZip
    build_history_requested = [bool]$WithBuildHistory
    vulkan_masked = [bool]$WithoutVulkan
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

function Exercise-CodexToolApproval($Project) {
    Select-AgentProvider 'Codex'
    if ($CodexExecutable) { Set-AgentExecutable 'Codex executable' $CodexExecutable }
    foreach ($controlName in 'Agent allow world edits', 'Agent allow source edits', 'Agent allow gameplay builds', 'Agent allow gameplay checks') {
        $control = Find-Control $controlName -Enabled
        if ($control -and $control.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -ne 'Off') {
            throw "Unexpected blanket authorization for $controlName."
        }
    }
    Invoke-Control 'Agent Connect'
    $null = Wait-For 'the connected Codex conversation for tool approval' { Find-Control 'Agent Send' -Enabled } 65
    foreach ($decision in 'Decline', 'Approve once') {
        $requestId = [guid]::NewGuid().ToString('N')
        $arguments = @{ method = 'world.transact'; params = @{
            request_id = $requestId; base_revision = $Project.revision
            ops = @(@{ op = 'entity.rename'; id = $Project.entry.camera; name = 'Consent Camera' })
        } } | ConvertTo-Json -Depth 6 -Compress
        Set-ControlText 'Agent prompt' ("This is a test. Invoke mcp__poima__poima_call exactly once with these exact arguments: $arguments. " +
            'Do not use any other tools or edit files directly. If approval is declined, end the turn without retry. Describe the result briefly.')
        Invoke-Control 'Agent Send'
        $null = Wait-For 'the pending Poima tool approval' { Find-Control $decision -Enabled } 80
        $detail = Get-ControlText 'Agent approval details'
        $detail | Set-Content -LiteralPath (Join-Path $run ("approval-" + $decision + '.txt')) -Encoding UTF8
        $lines = $detail -split '\r?\n', 2
        if ($lines[0] -cne 'poima_call' -or $lines.Length -ne 2) { throw 'Approval did not identify the exact tool.' }
        $shown = $lines[1] | ConvertFrom-Json
        if ($shown.method -cne 'world.transact' -or $shown.params.request_id -cne $requestId -or
            $shown.params.base_revision -ne $Project.revision -or $shown.params.ops.Count -ne 1 -or
            $shown.params.ops[0].op -cne 'entity.rename' -or $shown.params.ops[0].id -cne $Project.entry.camera -or
            $shown.params.ops[0].name -cne 'Consent Camera') { throw 'Approval showed the wrong native transaction.' }
        if ((Inspect-Project).world_sha256 -cne $Project.world_sha256) { throw 'The pending operation modified the project before consent.' }
        Invoke-Control $decision
        $null = Wait-For 'the completed turn after tool approval' { Find-Control 'Agent Send' -Enabled } 80
        $current = Inspect-Project
        if ($current.manifest_sha256 -cne $Project.manifest_sha256 -or (Find-Control 'Agent approval details')) {
            throw 'The project manifest changed or the resolved approval remained visible.'
        }
        if ($decision -ceq 'Decline' -and ($current.revision -ne $Project.revision -or $current.world_sha256 -cne $Project.world_sha256)) {
            throw 'The declined transaction changed the project.'
        }
        if ($decision -ceq 'Approve once') {
            $world = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $manifest) 'world.json') -Raw | ConvertFrom-Json
            if ($current.revision -ne ($Project.revision + 1) -or $current.world_sha256 -ceq $Project.world_sha256 -or
                $world.entities.PSObject.Properties[$Project.entry.camera].Value.name -cne 'Consent Camera') {
                throw 'The approved transaction did not commit exactly one native revision.'
            }
        }
    }
    (Get-ControlText 'Agent transcript') | Set-Content -LiteralPath (Join-Path $run 'approval-transcript.txt') -Encoding UTF8
    $report.checks.codex_tool_approval = [ordered]@{ declined_revision = $Project.revision; approved_revision = $current.revision;
        pending_and_declined_world_unchanged = $true; exact_arguments_reviewed = $true; resolved_cards_removed = $true }
    Invoke-Control 'Agent Disconnect'
    $null = Wait-For 'the retired Codex approval connection' { Find-Control 'Agent Connect' -Enabled }
    return $current
}

function Exercise-CodexQuestions($Project, $Peer) {
    Select-AgentProvider 'Codex'
    $settings = Wait-For 'Connection settings' { Find-Control 'Connection settings' -Enabled }
    $expander = $settings.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $expander.Expand()
    Set-ControlText 'Codex executable' (Resolve-Path -LiteralPath $Peer).Path
    Set-ControlText "Model (empty uses the CLI's advertised default)" ''
    Set-ControlText 'Conversation ID (empty starts a new conversation)' ''
    $expander.Collapse()
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

function Check-UnavailablePreview($Project, $Phase) {
    Invoke-Control 'Play project'
    $message = Wait-For 'a driver diagnostic with Play available again' {
        $control = Find-ControlId 'preview-status'
        if ($control -and $control.Current.Name -like '*start the Vulkan game preview*graphics driver*' -and
            (Find-Control 'Play project' -Enabled)) { $control.Current.Name }
    }
    $state = Inspect-Desktop
    $current = Inspect-Project
    if ($state.runtime.active -or $state.views.game.attached -or $state.views.game.frames_presented -ne 0 -or
        $current.world_sha256 -cne $Project.world_sha256 -or $current.manifest_sha256 -cne $Project.manifest_sha256) {
        throw 'The failed graphics startup left a preview active or changed the project.'
    }
    $state | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $run "$Phase-no-graphics-state.json") -Encoding UTF8
    return [ordered]@{ diagnostic = $message; runtime_active = $state.runtime.active; game_attached = $state.views.game.attached;
        frames_presented = $state.views.game.frames_presented; unchanged_project = $true }
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

function Rename-ProjectCamera($Project, $CameraName) {
    $request = @{
        jsonrpc = '2.0'; id = 1; method = 'world.transact'
        params = @{
            request_id = [guid]::NewGuid().ToString('N'); base_revision = $Project.revision
            ops = @(@{ op = 'entity.rename'; id = $Project.entry.camera; name = $CameraName })
        }
    } | ConvertTo-Json -Depth 6 -Compress
    $response = $request | & $cli connect $endpoint --timeout-ms 2000 | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or !$response.result -or $response.result.revision -ne ($Project.revision + 1)) {
        throw 'The shared owner did not commit the camera edit.'
    }
    $saved = Inspect-Project
    if ($saved.revision -ne $response.result.revision -or $saved.world_sha256 -ceq $Project.world_sha256 -or
        $saved.manifest_sha256 -cne $Project.manifest_sha256) {
        throw 'The shared edit did not produce the expected saved project revision.'
    }
    return $saved
}

function Edit-ProjectForExport($Project, $CameraName = 'Export viewpoint') {
    $saved = Rename-ProjectCamera $Project $CameraName
    $report.checks.edit_before_export = [ordered]@{ revision = $saved.revision; world_sha256 = $saved.world_sha256 }
    return $saved
}

function Read-PlaytestHistory($ProjectId) {
    $path = Join-Path $projects (Join-Path '.poima-history' $ProjectId)
    $response = & $cli project history $path --project-id $ProjectId | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or !$response.result) { throw 'Project history could not be read.' }
    return $response.result
}

function Write-PlaytestSuite($Controller, $Range) {
    $suite = @{
        format = 'poima.game-verification'; version = 1
        groups = @(@{ name = 'Walk to the marker'; sequence = @(
            @{ step = @{ ticks = 15; inputs = @(@{ entity = $Controller; move = @(0, 1) }) } }
            @{ expect = @{
                label = 'The player reaches the marker'; method = 'runtime.entity'
                params = @{ id = $Controller }; path = '/world_matrix/14'; between = $Range
            } }
        ) })
    } | ConvertTo-Json -Depth 10 -Compress
    $path = Join-Path (Split-Path -Parent $manifest) 'game.verify.json'
    [IO.File]::WriteAllText($path, $suite)
}

function Inspect-PlaytestDialog($History, [bool]$Passed) {
    $attempt = $History.latest_check
    $creator = [IntPtr]$window.Current.NativeWindowHandle
    Invoke-Control 'View playtest results'
    $handle = [IntPtr](Wait-For 'playtest result dialog' {
        $found = [CreatorTestWindow]::FindDialog($creator, 'Playtest results')
        if ($found -ne [IntPtr]::Zero) { $found }
    })
    try {
        $window = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
        $status = if ($Passed) { 'passed' } else { 'failed' }
        $group = Wait-For 'the named group and result' { Find-Control "Walk to the marker ($status)" }
        $null = Wait-For 'the exact tested build' { Find-Control "Tested build $($attempt.bundle_sha256.Substring(0,12))" }
        if ($Passed) {
            $group.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
        } else {
            $working = $History.latest
            $null = Wait-For 'the still playable build' {
                Find-Control "Revision $($working.revision) is still playable ($($working.bundle_sha256.Substring(0,12)))."
            }
        }
        $null = Wait-For 'the named gameplay check' { Find-Control "The player reaches the marker ($status)" }
        Invoke-Control 'Done'
        $null = Wait-For 'results closed' {
            [CreatorTestWindow]::FindDialog($creator, 'Playtest results') -eq [IntPtr]::Zero
        }
    }
    finally {
        if ([CreatorTestWindow]::FindDialog($creator, 'Playtest results') -eq $handle) {
            $null = [CreatorTestWindow]::PostMessage($handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        }
    }
}

function Export-PlaytestCandidate($Project, [bool]$Passed) {
    Write-PlaytestSuite $Project.entry.controller $(if ($Passed) { @(0.5, 1.8) } else { @(100, 101) })
    Invoke-Control 'Export Windows build'
    $null = Wait-For 'the completed playtest control' { Find-Control 'View playtest results' -Enabled } 90
    $current = Read-PlaytestHistory $Project.project_id
    if (!$current.latest_check -or $current.latest_check.passed -ne $Passed -or
        $current.latest_check.revision -ne $Project.revision -or $current.latest_check.retained -ne $Passed) {
        throw 'The saved candidate did not match the playtest result.'
    }
    Inspect-PlaytestDialog $current $Passed
    return $current
}

function Reopen-PlaytestResults($Project, $FailedHistory) {
    $null = Wait-For 'the restored playtest result and verified play controls' {
        (Find-Control 'View playtest results' -Enabled) -and (Find-Control 'Play verified build' -Enabled)
    } 60
    $current = Read-PlaytestHistory $Project.project_id
    if ($current.latest_check.report_sha256 -cne $FailedHistory.latest_check.report_sha256 -or
        $current.latest.bundle_sha256 -cne $FailedHistory.latest.bundle_sha256) {
        throw 'Reopening selected a different test attempt or playable build.'
    }
    Inspect-PlaytestDialog $current $false
    $path = $current.latest_check.report
    $exact = [IO.File]::ReadAllBytes($path)
    try {
        [IO.File]::AppendAllText($path, "`n")
        Invoke-Control 'View playtest results'
        $null = Wait-For 'report digest rejection' { Find-Control 'The playtest report changed. Reopen the project to refresh its results.' }
    }
    finally { [IO.File]::WriteAllBytes($path, $exact) }
    $runtime = Join-Path (Split-Path -Parent $current.latest.game_manifest) 'runtime\bin\poima.exe'
    Invoke-Control 'Play verified build'
    $player = Wait-For 'the retained standalone player window' {
        Get-Process -Name poima -ErrorAction SilentlyContinue | Where-Object {
            $_.Path -ieq $runtime -and $_.MainWindowHandle -ne [IntPtr]::Zero
        } | Select-Object -First 1
    } 45
    $script:verifiedPlayer = $player
    $null = $player.Handle
    if (![CreatorTestWindow]::PostMessage($player.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) -or
        !$player.WaitForExit(20000) -or $player.ExitCode -ne 0) {
        throw 'The retained standalone player failed to close normally.'
    }
    Invoke-Control 'Restore verified project as copy'
    $null = Wait-For 'an independently editable recovered copy' { Find-Control 'Open recovered project' -Enabled } 60
    $copies = @(Get-ChildItem -LiteralPath $projects -Directory -Filter "$name - Recovered *")
    if ($copies.Count -ne 1) { throw 'Expected one restored project copy.' }
    $copy = & $cli project inspect (Join-Path $copies[0].FullName 'project.poimaproject') | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $copy.result.project_id -ceq $Project.project_id -or
        $copy.result.revision -ne $current.latest.revision) { throw 'Recovery did not restore the passing revision.' }
    return [ordered]@{
        failed_report_sha256 = $current.latest_check.report_sha256
        failed_revision = $current.latest_check.revision
        playable_bundle_sha256 = $current.latest.bundle_sha256
        playable_revision = $current.latest.revision
        report_changed_rejected = $true
        verified_player_exit = $player.ExitCode
        recovered_revision = $copy.result.revision
    }
}

function Select-BuildVersion($Entry) {
    $verdict = if ($Entry.passed) { 'passed' } else { 'failed' }
    $label = "Revision $($Entry.revision) $verdict ($($Entry.bundle_sha256.Substring(0,12)))"
    $item = Wait-For $label { Find-Control $label }
    $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
    while ($item -and $item.Current.ControlType -ne [System.Windows.Automation.ControlType]::ListItem) {
        $item = $walker.GetParent($item)
    }
    if (!$item) { throw 'The history row was not selectable.' }
    $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
}

function Restore-OlderBuild($Project, $Older, $Latest) {
    $previous = @(Get-ChildItem -LiteralPath $projects -Directory -Filter "$name - Recovered *" | ForEach-Object FullName)
    $creator = [IntPtr]$window.Current.NativeWindowHandle
    Invoke-Control 'Build history'
    $handle = Wait-For 'build history window' {
        $found = [CreatorTestWindow]::FindDialog($creator, 'Build history')
        if ($found -ne [IntPtr]::Zero) { $found }
    }
    $window = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
    Select-BuildVersion $Latest.latest_check
    if (Find-Control 'Restore this version as copy' -Enabled) { throw 'Build history offered to restore a failed version.' }
    Invoke-Control 'View checks'
    $detail = Wait-For 'historical playtest results' {
        $found = [CreatorTestWindow]::FindDialog($handle, 'Playtest results')
        if ($found -ne [IntPtr]::Zero) { $found }
    }
    & {
        $window = [System.Windows.Automation.AutomationElement]::FromHandle($detail)
        $null = Wait-For 'historical failed check group' { Find-Control 'Walk to the marker (failed)' }
        Invoke-Control 'Done'
    }
    $null = Wait-For 'return to build history' { Find-Control 'View checks' -Enabled }
    Select-BuildVersion $Older.latest_check
    Invoke-Control 'Restore this version as copy'

    $copy = Wait-For 'a copy of the selected older build' {
        Get-ChildItem -LiteralPath $projects -Directory -Filter "$name - Recovered *" |
            Where-Object { $_.FullName -notin $previous } | Select-Object -First 1
    } 60
    $manifestPath = Join-Path $copy.FullName 'project.poimaproject'
    $restored = (& $cli project inspect $manifestPath | ConvertFrom-Json).result
    $before = (& $cli project inspect $manifest | ConvertFrom-Json).result
    $live = Read-PlaytestHistory $Project.project_id
    $source = Get-Content -LiteralPath $Older.latest.checkpoint -Raw | ConvertFrom-Json
    $world = $source.files | Where-Object { $_.path -ceq 'source/world.json' }
    if ($restored.project_id -ceq $Project.project_id -or $restored.revision -ne $Older.latest.revision -or
        $restored.world_sha256 -cne $world.sha256 -or $restored.revision -ge $Latest.latest.revision -or
        $before.manifest_sha256 -cne $Project.manifest_sha256 -or $before.world_sha256 -cne $Project.world_sha256 -or
        $live.latest.bundle_sha256 -cne $Latest.latest.bundle_sha256) {
        throw 'The history restore did not reproduce the selected older build and preserve the working project.'
    }
    return [ordered]@{
        failed_version_disabled_restore = $true
        failed_checks_opened = $true
        restored_revision = $restored.revision
        newer_passing_revision = $Latest.latest.revision
        restored_world_sha256 = $restored.world_sha256
        selected_bundle_sha256 = $Older.latest.bundle_sha256
        latest_bundle_sha256 = $live.latest.bundle_sha256
        live_project_unchanged = $true
    }
}

function Save-GameZip($Archive) {
    $creator = [IntPtr]$window.Current.NativeWindowHandle
    Invoke-Control 'Save game ZIP'
    $handle = [IntPtr](Wait-For 'the owned game ZIP save dialog' {
        $found = [CreatorTestWindow]::FindDialog($creator, 'Save game ZIP')
        if ($found -ne [IntPtr]::Zero) { $found }
    })
    try {
        $window = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
        if (!$Archive) { Invoke-Control 'Cancel' }
        else {
            $field = Wait-For 'the archive filename field' { Find-ControlId '1001' }
            $field.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($Archive)
            Invoke-Control 'Save'
        }
        $null = Wait-For 'the save dialog to close' {
            [CreatorTestWindow]::FindDialog($creator, 'Save game ZIP') -eq [IntPtr]::Zero
        }
    }
    finally {
        if ([CreatorTestWindow]::FindDialog($creator, 'Save game ZIP') -eq $handle) {
            $null = [CreatorTestWindow]::PostMessage($handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        }
    }
}

function Play-RelocatedExport($Bundle, $Expected = $report.checks.export, $Kind = 'export') {
    $source = Split-Path -Parent $manifest
    if (Test-Path -LiteralPath $source) {
        Move-Item -LiteralPath $source -Destination (Join-Path $run 'retired source')
    }
    if (Test-Path -LiteralPath $manifest) { throw 'The original project remains available.' }
    $relocated = Join-Path $run "Relocated Windows $Kind"
    Move-Item -LiteralPath $Bundle -Destination $relocated
    $gameManifest = Join-Path $relocated 'game.json'
    $replay = Join-Path $run "$Kind-replay.json"
    $capture = Join-Path $run "$Kind-frame.bmp"
    $playerReport = Join-Path $run "$Kind-player.json"
    $launcher = Join-Path $relocated 'launch.cmd'
    $localData = Join-Path $run "$Kind player local data"
    New-Item -ItemType Directory -Path $localData | Out-Null
    '[{"ticks":12,"move":[0,1]}]' | Set-Content -LiteralPath $replay -Encoding ASCII
    $path = $env:PATH
    $originalLocalData = $env:LOCALAPPDATA
    Push-Location $run
    try {
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        $env:LOCALAPPDATA = $localData
        & $launcher --replay $replay --width 640 --height 360 --capture $capture --report $playerReport |
            Set-Content -LiteralPath (Join-Path $run "$Kind-player.stdout.txt")
        $exitCode = $LASTEXITCODE
    }
    finally {
        $env:PATH = $path
        $env:LOCALAPPDATA = $originalLocalData
        Pop-Location
    }
    if ($exitCode -ne 0) { throw "The relocated player exited with $exitCode." }
    $result = Get-Content -LiteralPath $playerReport -Raw | ConvertFrom-Json
    $saveRoot = Join-Path $localData "Poima\Games\$($Expected.project_id)\Saves"
    if (!$result.play.success -or !$result.play.hardware -or $result.play.tick -ne 12 -or
        $result.play.frames_presented -lt 1 -or $result.play.stop_reason -cne 'replay_complete' -or
        $result.play.nvrhi_errors -ne 0 -or !(Test-Path -LiteralPath $capture) -or !(Test-Path -LiteralPath $saveRoot -PathType Container) -or
        (Get-FileHash -LiteralPath $gameManifest).Hash.ToLowerInvariant() -cne $Expected.manifest_sha256) {
        throw 'Native playback of the relocated export failed.'
    }
    $report.checks["relocated_$Kind"] = [ordered]@{
        original_project_unavailable = !(Test-Path -LiteralPath $manifest)
        exit_code = $exitCode
        tick = $result.play.tick
        presented_frames = $result.play.frames_presented
        gpu = $result.play.gpu
        external_save_directory_created = $true
        capture_sha256 = (Get-FileHash -LiteralPath $capture).Hash.ToLowerInvariant()
    }
}

$oldDriverFiles, $oldIcdFiles = $env:VK_DRIVER_FILES, $env:VK_ICD_FILENAMES
try {
    if ($WithoutVulkan) {
        $env:VK_DRIVER_FILES = $env:VK_ICD_FILENAMES = Join-Path $run 'missing-driver.json'
    }
    $owner = Start-Launcher 'create'
    $window = Wait-For 'the launcher window' { Find-Window 'Poima' }
    if ($WithoutVulkan) {
        $homeWindow = $window
        $homeHandle = [IntPtr]$homeWindow.Current.NativeWindowHandle
        Invoke-Control 'Set up gameplay builds'
        $dialog = Wait-For 'the gameplay setup window' {
            $handle = [CreatorTestWindow]::FindDialog($homeHandle, 'Set up gameplay builds')
            if ($handle -ne [IntPtr]::Zero) { $handle }
        }
        $window = [Windows.Automation.AutomationElement]::FromHandle($dialog)
        Invoke-Control 'Done'
        $window = $homeWindow
        $report.checks.gameplay_setup_opened = $true
    }
    $field = Wait-For 'Project name' { Find-Control 'Project name' -Enabled }
    $field.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($name)
    Invoke-Control 'Create project'
    $window = Wait-For 'the creator window' { Find-Window "$name*Poima Creator" }
    $project = Inspect-Project
    $endpoint = 'poima-' + $project.project_id
    $report.checks.created = [ordered]@{ id = $project.project_id; revision = $project.revision }

    if ($WithoutVulkan) {
        $report.checks.first_preview_failure = Check-UnavailablePreview $project 'first'
        $project = Rename-ProjectCamera $project 'Authored after driver failure'
        $report.checks.authoring_after_graphics_failure = [ordered]@{ revision = $project.revision; world_sha256 = $project.world_sha256 }
    }
    else {
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
    }
    if ($WithExport) {
        $project = Edit-ProjectForExport $project
        $bundle = Export-Project $project
    }
    if ($WithPlaytestResults -or $WithShareZip -or $WithBuildHistory) {
        if ($WithShareZip -and (Find-Control 'Save game ZIP' -Enabled)) {
            throw 'A project with no verified build can offer a verified ZIP.'
        }
        $passing = Export-PlaytestCandidate $project $true
        if ($WithBuildHistory) {
            $olderPassing = $passing
            $project = Edit-ProjectForExport $project 'Second verified viewpoint'
            $passing = Export-PlaytestCandidate $project $true
        }
        $project = Edit-ProjectForExport $project 'Playtested viewpoint'
        $failedHistory = Export-PlaytestCandidate $project $false
        if ($failedHistory.latest.bundle_sha256 -cne $passing.latest.bundle_sha256) {
            throw 'The failed attempt replaced the playable version.'
        }
    }
    if ($Antigravity) { Exercise-Acp $project 'Antigravity' 'Antigravity CLI executable' $Antigravity 'antigravity' }
    if ($Claude) { Exercise-Acp $project 'Claude Code' 'Claude ACP executable' $Claude 'claude' }
    if ($CodexBrief) { Exercise-CodexBrief $project }
    if ($CodexToolApproval) { $project = Exercise-CodexToolApproval $project }
    if ($WithCodex) {
        Select-AgentProvider 'Codex'
        if ($CodexExecutable) { Set-AgentExecutable 'Codex executable' $CodexExecutable }
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
        if ($WithCodex) {
            Invoke-Control 'Agent Disconnect'
            $null = Wait-For 'the retired Codex connection' { Find-Control 'Agent Connect' -Enabled }
        }
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
    if ($WithPlaytestResults -or $WithShareZip -or $WithBuildHistory) {
        $report.checks.playtest_results = Reopen-PlaytestResults $reopened $failedHistory
    }
    if ($WithBuildHistory) { $report.checks.build_history = Restore-OlderBuild $reopened $olderPassing $failedHistory }
    if ($WithShareZip) {
        $archive = Join-Path $run ('Recipient & Game ' + [char]0x03a9 + '.zip')
        Save-GameZip $null
        $null = Wait-For 'creator ready after cancelling save' { Find-Control 'Save game ZIP' -Enabled }
        if ((Find-Control 'Open ZIP folder') -or (Test-Path -LiteralPath $archive)) {
            throw 'Cancelling the dialog unexpectedly published a game ZIP.'
        }
        Save-GameZip $archive
        $null = Wait-For 'the saved ZIP and its folder button' {
            (Test-Path -LiteralPath $archive -PathType Leaf) -and (Find-Control 'Open ZIP folder' -Enabled)
        } 60
        $report.checks.share_zip = [ordered]@{
            cancelled_without_output = $true
            revision = $passing.latest.revision
            bundle_sha256 = $passing.latest.bundle_sha256
            archive_sha256 = (Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant()
            archive_bytes = (Get-Item -LiteralPath $archive).Length
        }
    }
    if ($WithoutVulkan) {
        $report.checks.reopened_preview_failure = Check-UnavailablePreview $reopened 'reopened'
        Close-Owner
        $report.checks.closed_after_graphics_failure = $owner.ExitCode
    } else {
        $report.checks.reopened_preview = Play-Preview $reopened.revision
        if ($report.checks.reopened_preview.session -eq $report.checks.first_preview.session) {
            throw 'Reopening reused the retired play session.'
        }
        Close-Owner
        $report.checks.closed_during_play_exit_code = $owner.ExitCode
    }
    if ($WithExport) { Play-RelocatedExport $bundle }
    if ($WithShareZip) {
        $archiveRoot = Join-Path $projects '.poima-history'
        Move-Item -LiteralPath $archiveRoot -Destination (Join-Path $run 'retired history')
        if (Test-Path -LiteralPath $passing.latest.game_manifest) { throw 'The archived original remains available.' }
        $game = Join-Path $run 'Extracted ZIP'
        Expand-Archive -LiteralPath $archive -DestinationPath $game
        Play-RelocatedExport $game @{ project_id = $project.project_id; manifest_sha256 = $passing.latest.bundle_sha256 } 'zip'
    }
    $report.passed = $true
}
catch {
    $report.error = $_.ToString()
    throw
}
finally {
    $env:VK_DRIVER_FILES, $env:VK_ICD_FILENAMES = $oldDriverFiles, $oldIcdFiles
    if ($verifiedPlayer -and !$verifiedPlayer.HasExited) {
        $report.forced_player_cleanup = $true
        Stop-Process -Id $verifiedPlayer.Id
    }
    if ($owner -and !$owner.HasExited) {
        $report.forced_cleanup = $true
        Stop-Process -Id $owner.Id
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'report.json') -Encoding UTF8
    Write-Output $run
}
