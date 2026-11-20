# SPDX-License-Identifier: Apache-2.0
# Run in the interactive Windows session against a portable desktop directory.
# UI Automation addresses only windows owned by this run. The run uses no
# physical input or pre-existing project. -WithCodex opens a local
# Codex connection with the installed client and submits no model turn.
# -CodexExecutable selects an exact client for that installed-client check.
# -Antigravity and -Claude use the supplied ACP clients for one MCP read each.
# -InstallAntigravity downloads the official CLI and checks the browser sign-in choices.
# -AntigravitySignInPeer exercises a simulated browser authorization through the supplied ACP fixture.
# -InstallClaude downloads the official adapter into an unused per-user install folder and uses an isolated signed-out profile.
# -CodexBrief runs a two-turn interview on the unchanged starter project.
# -CodexQuestions uses the local AgentRpc contract executable as a scripted app-server.
# -CodexReviews uses the AgentRpc contract executable to test two revision-pinned reviewers.
# -CodexToolApproval exercises decline and approve once with the installed Codex.
# -WithExport requires a packaged sample runtime and plays a relocated export.
# -WithPlaytestResults checks passing and failed reports, reopening and recovery.
# -WithShareZip also exercises the owned save dialog and plays the unpacked ZIP.
# -WithBuildHistory creates two passing versions and restores the older one through the history dialog.
# -VerifiedPlayChat tests live workspace chat and native candidate publication while playing a verified build.
# -VerifiedPlayWithCodex uses an installed, signed-in Codex model to author and retain a new version during playback.
# -ObserveVerifiedPlayback checks the live progress and closure read through the same project endpoint.
# -WithoutVulkan masks drivers for the owned processes and checks preview failure recovery.
# -AssetFixtures imports model, color/data image and WAV fixtures through the file picker.
# -CodexAssetRead asks the real Codex client to inspect the model ID in the draft.
param(
    [Parameter(Mandatory = $true)][string]$Editor,
    [Parameter(Mandatory = $true)][string]$Output,
    [switch]$WithCodex,
    [string]$CodexExecutable,
    [string]$Antigravity,
    [switch]$InstallAntigravity,
    [string]$AntigravitySignInPeer,
    [string]$Claude,
    [switch]$InstallClaude,
    [switch]$CodexBrief,
    [string]$CodexQuestions,
    [string]$CodexReviews,
    [switch]$CodexToolApproval,
    [switch]$WithExport,
    [switch]$WithPlaytestResults,
    [switch]$WithShareZip,
    [switch]$WithBuildHistory,
    [string]$VerifiedPlayChat,
    [switch]$VerifiedPlayWithCodex,
    [switch]$ObserveVerifiedPlayback,
    [switch]$WithoutVulkan,
    [string]$AssetFixtures,
    [switch]$CodexAssetRead
)

$ErrorActionPreference = 'Stop'
if ($CodexAssetRead -and !$AssetFixtures) { throw 'Supply -AssetFixtures for the Codex asset handoff check.' }
if ($InstallClaude -and $Claude) { throw 'Run the signed-out installation and the supplied Claude client checks separately.' }
if ($InstallAntigravity -and $Antigravity) { throw 'Run the installation and supplied Antigravity client checks separately.' }
if ($VerifiedPlayChat -and $VerifiedPlayWithCodex) { throw 'Run the scripted and live playback checks separately.' }
if ($ObserveVerifiedPlayback -and !$VerifiedPlayWithCodex) { throw 'Use -VerifiedPlayWithCodex for the live observation workflow.' }
if ($CodexReviews -and ($WithCodex -or $CodexQuestions -or $CodexBrief -or $CodexToolApproval -or
    $VerifiedPlayChat -or $VerifiedPlayWithCodex -or $CodexAssetRead)) {
    throw 'Run the specialist review fixture separately from the other Codex scenarios.'
}
if ($VerifiedPlayChat -or $VerifiedPlayWithCodex) { $WithPlaytestResults = $true }
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
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr window, WindowCallback callback, IntPtr context);
    [DllImport("user32.dll")] private static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll")] private static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetClassName(IntPtr window, System.Text.StringBuilder name, int capacity);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern IntPtr SendMessageTimeout(IntPtr window, uint message,
        IntPtr wparam, string value, uint flags, uint timeout, out IntPtr result);
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
    public static bool ChooseFile(IntPtr dialog, string file) {
        IntPtr field = IntPtr.Zero;
        EnumChildWindows(dialog, (window, _) => {
            var name = new System.Text.StringBuilder(32);
            GetClassName(window, name, name.Capacity);
            if (name.ToString() != "Edit" ||
                (GetDlgCtrlID(window) != 1148 && GetDlgCtrlID(GetParent(window)) != 1148)) return true;
            field = window;
            return false;
        }, IntPtr.Zero);
        IntPtr accepted;
        if (field == IntPtr.Zero || SendMessageTimeout(field, 0x000c, IntPtr.Zero, file, 3, 5000, out accepted) == IntPtr.Zero ||
            accepted == IntPtr.Zero) return false;
        var open = GetDlgItem(dialog, 1);
        return open != IntPtr.Zero && PostMessage(open, 0x00f5, IntPtr.Zero, IntPtr.Zero);
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
    antigravity_install_requested = [bool]$InstallAntigravity
    antigravity_sign_in_fixture_requested = ![string]::IsNullOrEmpty($AntigravitySignInPeer)
    claude_turn_requested = ![string]::IsNullOrEmpty($Claude)
    claude_install_requested = [bool]$InstallClaude
    codex_brief_requested = [bool]$CodexBrief
    codex_questions_requested = ![string]::IsNullOrWhiteSpace($CodexQuestions)
    codex_reviews_requested = ![string]::IsNullOrWhiteSpace($CodexReviews)
    export_requested = [bool]$WithExport
    playtest_results_requested = [bool]$WithPlaytestResults
    share_zip_requested = [bool]$WithShareZip
    build_history_requested = [bool]$WithBuildHistory
    verified_play_chat_requested = [bool]$VerifiedPlayChat
    verified_play_with_codex_requested = [bool]$VerifiedPlayWithCodex
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

function Find-OwnedDialog($Parent, $Title) {
    $handle = [CreatorTestWindow]::FindDialog([IntPtr]$Parent.Current.NativeWindowHandle, $Title)
    if ($handle -ne [IntPtr]::Zero) {
        $dialog = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
        if ($dialog.Current.ProcessId -eq $owner.Id) { return $dialog }
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

function Select-ComboValue($Label, $Name) {
    $selector = Wait-For $Label { Find-Control $Label -Enabled }
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

function Select-AgentProvider($Name) { Select-ComboValue 'Agent provider' $Name }

function Set-AgentExecutable($Control, $Path, [switch]$AllowMissing) {
    $settings = Wait-For 'Connection settings' { Find-Control 'Connection settings' -Enabled }
    $settings.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Expand()
    $absolute = if ($AllowMissing) { [System.IO.Path]::GetFullPath($Path) } else { (Resolve-Path -LiteralPath $Path).Path }
    Set-ControlText $Control $absolute
    $settings.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern).Collapse()
}

function Send-CodexPrompt($Prompt, [scriptblock]$AfterSubmit) {
    Set-ControlText 'Agent prompt' $Prompt
    Invoke-Control 'Agent Send'
    $prefix = "You: $Prompt"
    $null = Wait-For 'the submitted Codex prompt' {
        (Get-ControlText 'Agent transcript').Contains($prefix)
    }
    if ($AfterSubmit) { & $AfterSubmit }
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

function Open-CodexQuestionRound($Peer) {
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
    $null = Wait-For 'the first choice' { Find-ControlId 'agent-question-1-choice-1' }
}

function Answer-CodexQuestionRound {
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
}

function Exercise-CodexQuestions($Project, $Peer) {
    Open-CodexQuestionRound $Peer
    Answer-CodexQuestionRound
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

function Wait-ForCreatorChatTab($TabName, [switch]$Select) {
    $tab = Wait-For "the $TabName chat tab" { Find-Control $TabName -Enabled }
    $selection = $tab.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern)
    if ($Select) { $selection.Select() }
    $null = Wait-For "$TabName selected" { $selection.Current.IsSelected }
}

function Wait-ForReviewFinding($Finding) {
    $null = Wait-For "the completed finding '$Finding'" {
        (Find-ControlId 'specialist-status').Current.Name -ceq 'Review complete' -and
            (Get-ControlText 'Specialist findings') -ceq $Finding
    } 65
}

function Start-ReviewTask($TaskText) {
    Set-ControlText 'Specialist task' $TaskText
    Invoke-Control 'Send to specialist'
}

function Wait-ForReviewDraft($DraftText) {
    $null = Wait-For "the active review streaming '$DraftText'" {
        (Find-Control 'Stop review' -Enabled) -and (Get-ControlText 'Specialist findings') -ceq $DraftText
    } 65
}

function Read-ReviewTrace {
    Get-ChildItem -LiteralPath $reviewTrace -Filter 'codex-*.jsonl' -File | ForEach-Object {
        Get-Content -LiteralPath $_.FullName -Encoding UTF8 -ErrorAction SilentlyContinue |
            ConvertFrom-Json -ErrorAction SilentlyContinue
    }
}

function Find-ReviewTurn($TaskText) {
    Read-ReviewTrace | Where-Object {
        $_.kind -ceq 'turn/start' -and $_.text.EndsWith($TaskText, [StringComparison]::Ordinal)
    }
}

function Wait-ForReviewExit($ProcessId) {
    $null = Wait-For "reviewer $ProcessId exiting after stdin closes" {
        !(Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) -and
            (Read-ReviewTrace | Where-Object { $_.pid -eq $ProcessId -and $_.kind -ceq 'exit' })
    }
}

function Exercise-CodexReviews($Project, $Peer) {
    $checks = $report.checks.codex_reviews = [ordered]@{ original_revision = $Project.revision }
    if (Find-Control 'Specialist reviews' -Enabled) { throw 'Specialist reviews can open before Codex connects.' }
    Select-AgentProvider 'Codex'
    Set-AgentExecutable 'Codex executable' $Peer
    Invoke-Control 'Agent Connect'
    $null = Wait-For 'the scripted lead Codex connection' { Find-Control 'Agent Send' -Enabled } 65
    $originalDraft = 'Keep this lead draft.'
    Set-ControlText 'Agent prompt' $originalDraft
    $leadTranscript = Get-ControlText 'Agent transcript'
    Invoke-Control 'Specialist reviews'
    Wait-ForCreatorChatTab 'Specialist 1'
    $checks.same_workspace_chat_tabs = $true
    $worldPath = Join-Path (Split-Path -Parent $manifest) $Project.entry.world
    $worldId = (Get-Content -LiteralPath $worldPath -Raw | ConvertFrom-Json).world_id
    $null = Wait-For 'the original review revision' {
        (Find-ControlId 'specialist-scope').Current.Name -clike "Reviewing revision $($Project.revision).*"
    }
    if (Find-Control 'Use findings in chat' -Enabled) { throw 'An empty review can be handed to chat.' }
    $crateTask = 'Review the crate position.'
    $lightTask = 'Review the light intensity.'
    $followUpTask = 'Explain the crate finding.'
    $slowTask = 'Wait for cancellation.'
    $slowDraft = 'Waiting for cancellation.'
    $lightFinding = 'The light is too bright.'
    $followUpFinding = 'The crate blocks the doorway.'

    Start-ReviewTask $crateTask
    Wait-ForReviewDraft 'Reviewing the crate.'
    $crateTurn = Find-ReviewTurn $crateTask
    if (!$crateTurn) { throw 'The first specialist turn was not recorded.' }
    Wait-ForCreatorChatTab 'Main chat' -Select
    $null = Wait-For 'the lead chat available during the review' { Find-Control 'Agent Send' -Enabled }
    if ((Get-ControlText 'Agent prompt') -cne $originalDraft -or (Get-ControlText 'Agent transcript') -cne $leadTranscript) {
        throw 'The active specialist changed the lead draft or transcript.'
    }
    Wait-ForCreatorChatTab 'Specialist 2' -Select
    Start-ReviewTask $lightTask
    Wait-ForReviewFinding $lightFinding
    Wait-ForCreatorChatTab 'Specialist 1' -Select
    if (!(Find-Control 'Stop review' -Enabled) -or (Get-ControlText 'Specialist findings') -cne 'Reviewing the crate.' -or
        (Find-Control 'Use findings in chat' -Enabled)) {
        throw 'The second specialist did not finish independently while the first specialist was still streaming.'
    }
    Wait-ForCreatorChatTab 'Main chat' -Select
    [IO.File]::WriteAllText((Join-Path $reviewTrace 'release-crate'), '')
    $null = Wait-For 'the first review completing while Main chat is selected' {
        Read-ReviewTrace | Where-Object {
            $_.pid -eq $crateTurn.pid -and $_.turn -ceq $crateTurn.turn -and $_.kind -ceq 'turn/completed'
        }
    }
    Wait-ForCreatorChatTab 'Main chat'
    $checks.review_completed_while_main_chat_selected = $true
    Wait-ForCreatorChatTab 'Specialist 1' -Select
    Wait-ForReviewFinding 'The crate needs more clearance.'
    $findings = Find-Control 'Specialist findings'
    if (!$findings.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern).Current.IsReadOnly) {
        throw 'Specialist findings are editable.'
    }
    $checks.independent_streaming = $true
    Start-ReviewTask $followUpTask
    Wait-ForReviewFinding $followUpFinding
    Wait-ForCreatorChatTab 'Specialist 2' -Select
    Wait-ForReviewFinding $lightFinding

    $lightTurn = Find-ReviewTurn $lightTask
    $followUpTurn = Find-ReviewTurn $followUpTask
    if (!$lightTurn -or !$followUpTurn -or $crateTurn.pid -eq $lightTurn.pid -or
        $crateTurn.thread -ceq $lightTurn.thread -or $followUpTurn.pid -ne $crateTurn.pid -or
        $followUpTurn.thread -cne $crateTurn.thread -or $followUpTurn.turn -ceq $crateTurn.turn) {
        throw 'The two reviewers or the follow-up did not use the expected independent processes, threads and turns.'
    }
    $checks.reviewers = @($crateTurn, $lightTurn, $followUpTurn)

    Invoke-Control 'Use findings in chat'
    Wait-ForCreatorChatTab 'Main chat'
    $checks.handoff_selected_main_chat = $true
    $draft = Wait-For 'both findings in the unsent lead draft' {
        [string]$text = Get-ControlText 'Agent prompt'
        if ($text.StartsWith($originalDraft) -and $text.Contains($followUpFinding) -and $text.Contains($lightFinding) -and
            $text.Contains("world $worldId, revision $($Project.revision).")) { $text }
    }
    $draft | Set-Content -LiteralPath (Join-Path $run 'review-draft.txt') -Encoding UTF8
    $beforeSend = @(Read-ReviewTrace | Where-Object { $_.kind -ceq 'launch' -and $null -eq $_.revision })
    if ($beforeSend.Count -ne 1 -or (Get-ControlText 'Agent transcript') -cne $leadTranscript -or
        (Read-ReviewTrace | Where-Object { $_.pid -eq $beforeSend[0].pid -and $_.kind -ceq 'turn/start' })) {
        throw 'Handing off findings sent a lead turn before Agent Send.'
    }
    $checks.handoff_left_draft_unsent = $true
    Invoke-Control 'Agent Send'
    $null = Wait-For 'the lead responding to the explicit send' {
        (Find-Control 'Agent Send' -Enabled) -and
            (Get-ControlText 'Agent transcript').Contains('Lead received the findings.')
    } 65
    $leadTurn = @(Read-ReviewTrace | Where-Object { $_.pid -eq $beforeSend[0].pid -and $_.kind -ceq 'turn/start' })
    if ($leadTurn.Count -ne 1 -or ($leadTurn[0].text -cne $draft -and
        !$leadTurn[0].text.StartsWith($draft + "`n`n", [StringComparison]::Ordinal))) {
        throw 'The explicit send changed the handoff draft.'
    }
    (Get-ControlText 'Agent transcript') | Set-Content -LiteralPath (Join-Path $run 'review-lead-transcript.txt') -Encoding UTF8
    $checks.explicit_lead_send = $true

    Invoke-Control 'Specialist reviews'
    $null = Wait-For 'the existing specialist review visible again' { Find-ControlId 'specialist-scope' }
    Wait-ForCreatorChatTab 'Specialist 1' -Select
    Start-ReviewTask $slowTask
    Wait-ForReviewDraft $slowDraft
    Invoke-Control 'Stop review'
    $null = Wait-For 'the stopped review status' { (Find-ControlId 'specialist-status').Current.Name -ceq 'Review stopped' }
    Wait-ForReviewExit $crateTurn.pid
    Wait-ForCreatorChatTab 'Specialist 2' -Select
    Wait-ForReviewFinding $lightFinding
    if (!(Get-Process -Id $lightTurn.pid -ErrorAction SilentlyContinue)) { throw 'Stop retired the other reviewer.' }
    Invoke-Control 'Use findings in chat'
    Wait-ForCreatorChatTab 'Main chat'
    $remainingDraft = Wait-For 'only the remaining completed finding in the lead draft' {
        [string]$text = Get-ControlText 'Agent prompt'
        if ($text.Contains($lightFinding)) { $text }
    }
    if ($remainingDraft.Contains($slowDraft) -or $remainingDraft.Contains($followUpFinding)) {
        throw 'The stopped reviewer contributed a finding to the lead draft.'
    }
    Set-ControlText 'Agent prompt' ''
    Wait-ForCreatorChatTab 'Specialist 2' -Select
    $checks.stopped_one_reviewer = $true

    $saved = Inspect-Project
    if ($saved.revision -ne $Project.revision -or $saved.world_sha256 -cne $Project.world_sha256 -or
        $saved.manifest_sha256 -cne $Project.manifest_sha256) {
        throw 'The reviews or handoff changed the authored project.'
    }
    $refreshed = Rename-ProjectCamera $Project 'Camera changed between specialist reviews'
    Invoke-Control 'Use findings in chat'
    $null = Wait-For 'the stale findings refusal' {
        (Find-ControlId 'specialist-scope').Current.Name -ceq 'The scene changed. Review the current revision to continue.'
    }
    Wait-ForCreatorChatTab 'Main chat' -Select
    if (![string]::IsNullOrEmpty((Get-ControlText 'Agent prompt'))) { throw 'Stale findings reached the lead draft.' }
    Wait-ForCreatorChatTab 'Specialist 2' -Select
    Invoke-Control 'Review current revision'
    $null = Wait-For 'the refreshed revision and cleared findings' {
        (Find-ControlId 'specialist-scope').Current.Name -clike "Reviewing revision $($refreshed.revision).*" -and
            !(Find-Control 'Use findings in chat' -Enabled) -and
            [string]::IsNullOrEmpty((Get-ControlText 'Specialist findings'))
    }
    Wait-ForReviewExit $lightTurn.pid
    $checks.stale_handoff_rejected = $true
    $checks.refreshed_revision = $refreshed.revision

    Start-ReviewTask $slowTask
    Wait-ForReviewDraft $slowDraft
    $refreshedTurn = @(Find-ReviewTurn $slowTask | Where-Object { $_.pid -ne $crateTurn.pid })
    if ($refreshedTurn.Count -ne 1 -or $refreshedTurn[0].pid -eq $lightTurn.pid) {
        throw 'The fresh review did not start in a fresh process.'
    }
    $refreshedTurn = $refreshedTurn[0]

    $records = @(Read-ReviewTrace)
    foreach ($turn in $crateTurn, $lightTurn, $refreshedTurn) {
        $launch = $records | Where-Object { $_.pid -eq $turn.pid -and $_.kind -ceq 'launch' }
        $thread = $records | Where-Object { $_.pid -eq $turn.pid -and $_.kind -ceq 'thread/start' }
        $revision = if ($turn.pid -eq $refreshedTurn.pid) { $refreshed.revision } else { $Project.revision }
        if ($launch.endpoint -cne $endpoint -or $launch.revision -ne $revision -or
            $thread.thread -cne $turn.thread -or $thread.approvalPolicy -cne 'never' -or
            $thread.sandbox -cne 'read-only' -or $thread.ephemeral -ne $true -or
            !$turn.text.Contains("world $worldId at authored revision $revision.")) {
            throw 'A reviewer did not open a read-only, ephemeral thread at its assigned project revision.'
        }
    }
    $saved = Inspect-Project
    if ($saved.revision -ne $refreshed.revision -or $saved.world_sha256 -cne $refreshed.world_sha256 -or
        $saved.manifest_sha256 -cne $refreshed.manifest_sha256) {
        throw 'The refreshed review changed the authored project.'
    }
    $checks.refreshed_reviewer = $refreshedTurn
    $checks.pinned_read_only_threads = $true
    $checks.project_unchanged_by_reviews = $true
    Wait-ForCreatorChatTab 'Main chat' -Select
    $null = Wait-For 'the lead chat ready with an active specialist' { Find-Control 'Agent Send' -Enabled }
    $checks.lead_stayed_connected = $true
    $checks.lead_pid = $beforeSend[0].pid
    $checks.active_reviewer_before_creator_close = $refreshedTurn.pid
    return $refreshed
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

function Check-ClaudeSignIn {
    Select-AgentProvider 'Claude Code'
    Invoke-Control 'Agent Connect'
    $null = Wait-For 'Claude Code sign-in' { Find-Control 'Sign in to Claude Code' -Enabled } 65
    if (Find-Control 'Agent Send' -Enabled) { throw 'The signed-out client can send a turn.' }
}

function Check-AntigravitySignInChoices($Creator) {
    $null = Wait-For 'Antigravity connected or awaiting sign-in' {
        (Find-OwnedDialog $Creator 'Sign in to Antigravity') -or (Find-Control 'Sign in to Antigravity' -Enabled)
    } 65
    if (!(Find-OwnedDialog $Creator 'Sign in to Antigravity')) { Invoke-Control 'Sign in to Antigravity' }
    $window = Wait-For 'Antigravity sign-in window' { Find-OwnedDialog $Creator 'Sign in to Antigravity' }
    $null = Wait-For 'personal and enterprise sign-in' {
        (Find-Control 'Sign in with Google' -Enabled) -and (Find-Control 'Gemini Enterprise' -Enabled)
    }
    Invoke-Control 'Cancel'
    $window = $Creator
    $null = Wait-For 'creator after cancelled Antigravity sign-in' {
        !(Find-OwnedDialog $Creator 'Sign in to Antigravity') -and (Find-Control 'Agent Connect' -Enabled)
    }
    if (Find-Control 'Agent Send' -Enabled) { throw 'Cancelled sign-in left an active conversation.' }
}

function Exercise-AntigravityInstall($Project) {
    Select-AgentProvider 'Antigravity'
    Set-AgentExecutable 'Antigravity CLI executable' (Join-Path $run 'missing-agy.exe') -AllowMissing
    Invoke-Control 'Agent Connect'
    $null = Wait-For 'missing Antigravity recovery' { Find-Control 'Install Antigravity' -Enabled }
    $creator = $window
    Invoke-Control 'Install Antigravity'
    $null = Wait-For 'installed Antigravity' {
        if ($dialog = Find-OwnedDialog $creator 'Install Antigravity') {
            $condition = [System.Windows.Automation.PropertyCondition]::new(
                [System.Windows.Automation.AutomationElement]::NameProperty, 'Open setup guide')
            if ($dialog.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)) {
                $labels = $dialog.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                    [System.Windows.Automation.Condition]::TrueCondition)
                throw ('Antigravity installer failed. ' + (($labels | ForEach-Object { $_.Current.Name }) -join ' | '))
            }
        }
        (Find-OwnedDialog $creator 'Sign in to Antigravity') -or (Find-Control 'Agent Send' -Enabled)
    } 240
    $exePath = Join-Path $agyInstallRoot '1.5.13\agy.exe'
    $digest = (Get-FileHash -LiteralPath $exePath -Algorithm SHA512).Hash.ToLowerInvariant()
    if ($digest -cne '6b80a17a2bc34a969ed9ba31500a8201b0b4aad3e7fe061374022d161fec8bd7de49b6dc069a6ac67a90632e8671bbe860a7e94309bc5adbc536d27a75a86b58') {
        throw 'Installed Antigravity executable differs from the pinned download.'
    }
    if (Get-ChildItem -LiteralPath $agyInstallRoot -Force -Directory -Filter '.install-*') {
        throw 'Successful installation left staging files.'
    }
    $version = & $exePath --version
    if ($LASTEXITCODE -ne 0 -or $version -cne '1.5.13') { throw 'Unexpected Antigravity CLI version.' }
    Check-AntigravitySignInChoices $creator
    $current = Inspect-Project
    if ($current.manifest_sha256 -cne $Project.manifest_sha256 -or $current.world_sha256 -cne $Project.world_sha256) {
        throw 'Antigravity setup changed the project.'
    }
    $report.checks.antigravity_install = @{
        cli_sha512 = $digest; version = $version; sign_in_choices = $true; sign_in_cancelled = $true; project_unchanged = $true
    }
}

function Exercise-AntigravitySignInFixture($Peer) {
    Select-AgentProvider 'Antigravity'
    Set-AgentExecutable 'Antigravity CLI executable' $Peer
    $draft = 'Make a small lantern puzzle.'
    Set-ControlText 'Agent prompt' $draft
    $creator = $window
    Invoke-Control 'Agent Connect'
    $window = Wait-For 'synthetic signed-out recovery' { Find-OwnedDialog $creator 'Sign in to Antigravity' }
    Invoke-Control 'Sign in with Google'
    $window = $creator
    $null = Wait-For 'authenticated ACP fixture session' { Find-Control 'Agent Send' -Enabled }
    if ((Get-ControlText 'Agent prompt') -cne $draft) { throw 'The sign-in flow changed the draft.' }
    Set-ControlText 'Agent prompt' ''
    Invoke-Control 'Agent Disconnect'
    $null = Wait-For 'fixture disconnect' { Find-Control 'Agent Connect' -Enabled }
    $report.checks.antigravity_sign_in_fixture = @{ connected = $true; draft_unchanged = $true }
}

function Exercise-ClaudeInstall($Project) {
    Select-AgentProvider 'Claude Code'
    Set-AgentExecutable 'Claude ACP executable' (Join-Path $run 'missing-claude.exe') -AllowMissing
    Invoke-Control 'Agent Connect'
    $null = Wait-For 'missing adapter recovery' { Find-Control 'Install Claude Code' -Enabled }
    $creator = $window
    function Claude-Installer {
        Find-OwnedDialog $creator 'Install Claude Code'
    }
    Invoke-Control 'Install Claude Code'
    $window = Wait-For 'the installer window' { Claude-Installer }
    Invoke-Control 'Cancel'
    $null = Wait-For 'the cancelled installer to close' { !(Claude-Installer) }
    $window = $creator
    if (Test-Path -LiteralPath (Join-Path $claudeInstallRoot '0.106.0')) { throw 'Cancellation published an installation.' }
    if (Get-ChildItem -LiteralPath $claudeInstallRoot -Force -Directory -Filter '.install-*' -ErrorAction SilentlyContinue) {
        throw 'Cancelled installation left staging files.'
    }
    $report.checks.claude_install_cancelled = $true
    Invoke-Control 'Install Claude Code'
    $null = Wait-For 'the installed adapter sign-in' {
        if ($dialog = Claude-Installer) {
            $condition = [System.Windows.Automation.PropertyCondition]::new(
                [System.Windows.Automation.AutomationElement]::NameProperty, 'Open setup guide')
            if ($dialog.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)) {
                $labels = $dialog.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                    [System.Windows.Automation.Condition]::TrueCondition)
                throw ('Claude installer failed. ' + (($labels | ForEach-Object { $_.Current.Name }) -join ' | '))
            }
        }
        Find-Control 'Sign in to Claude Code' -Enabled
    } 240
    if (Find-Control 'Agent Send' -Enabled) { throw 'Installation bypassed sign-in.' }
    $installed = Join-Path $claudeInstallRoot '0.106.0'
    $adapter = (Get-FileHash -LiteralPath (Join-Path $installed 'claude-agent-acp.exe')).Hash.ToLowerInvariant()
    $cliHash = (Get-FileHash -LiteralPath (Join-Path $installed 'claude.exe')).Hash.ToLowerInvariant()
    if ($adapter -cne 'e8dac89c93f2003894eb375c0a6c5a2c3613e94629b6661d5a2ffd4dd4a60f88' -or
        $cliHash -cne '345996b4bdbae21d817f750fae2f34cbf8d81224a5444c78136091d121beb298') {
        throw 'Installed executables differ from the pinned archive.'
    }
    if (Get-ChildItem -LiteralPath $claudeInstallRoot -Force -Directory -Filter '.install-*') {
        throw 'Successful installation left staging files.'
    }
    $current = Inspect-Project
    if ($current.manifest_sha256 -cne $Project.manifest_sha256 -or $current.world_sha256 -cne $Project.world_sha256) {
        throw 'Claude Code setup changed the project.'
    }
    $report.checks.claude_install = @{
        adapter_sha256 = $adapter; cli_sha256 = $cliHash; sign_in_available = $true; project_unchanged = $true
    }
}

function Start-Launcher($Phase, [string]$ProjectPath) {
    $arguments = if ($ProjectPath) { "--creator `"$ProjectPath`"" } else { "--projects `"$projects`"" }
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $package -PassThru `
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
    if (!$owner.WaitForExit(20000)) {
        if ($CodexReviews) {
            $diagnostics = $report.checks.codex_review_close_diagnostics = [ordered]@{}
            try {
                $diagnostics.creator_status = (Find-ControlId 'preview-status').Current.Name
                $mainChat = Find-Control 'Main chat'
                if ($mainChat) {
                    $diagnostics.main_chat_selected = $mainChat.GetCurrentPattern(
                        [Windows.Automation.SelectionItemPattern]::Pattern).Current.IsSelected
                }
            } catch { $diagnostics.ui_error = $_.ToString() }
            try {
                $state = Inspect-Desktop
                $diagnostics.native_state = @{ closing = $state.closing; audio_closed = $state.audio.closed;
                    development_closed = $state.development_closed }
            } catch { $diagnostics.native_error = $_.ToString() }
        }
        throw 'The creator owner failed to retire.'
    }
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

function Select-CreatorAssetFile($Kind, $File, [switch]$Reject) {
    $creator = $window
    $creatorHandle = [IntPtr]$creator.Current.NativeWindowHandle
    Invoke-Control ('Import assets' + [char]0x2026)
    $dialog = [IntPtr](Wait-For 'the creator asset window' {
        $handle = [CreatorTestWindow]::FindDialog($creatorHandle, 'Import assets')
        if ($handle -ne [IntPtr]::Zero) { $handle }
    })
    $window = [Windows.Automation.AutomationElement]::FromHandle($dialog)
    Select-ComboValue 'Asset type' $Kind
    Invoke-Control ('Choose files' + [char]0x2026)
    $picker = [IntPtr](Wait-For 'the owned asset file picker' {
        $handle = [CreatorTestWindow]::FindDialog($dialog, 'Choose assets')
        if ($handle -ne [IntPtr]::Zero) { $handle }
    })
    if (![CreatorTestWindow]::ChooseFile($picker, $File)) { throw 'The test could not select the asset fixture.' }
    if ($Reject) {
        $null = Wait-For 'the rejected asset filename' {
            $control = Find-ControlId 'creator-import-status'
            if ($control -and $control.Current.Name -like ('*' + [IO.Path]::GetFileName($File) + '*') -and
                (Find-Control ('Choose files' + [char]0x2026) -Enabled)) { $control }
        }
        if (Find-Control 'Add to chat draft' -Enabled) { throw 'An unsuccessful import enabled the handoff.' }
        Invoke-Control 'Done'
    } else { Invoke-Control 'Add to chat draft' }
    $null = Wait-For 'return to the creator workspace' {
        [CreatorTestWindow]::FindDialog($creatorHandle, 'Import assets') -eq [IntPtr]::Zero
    }
}

function Exercise-CreatorAssets($Project) {
    $intro = 'Please use these files in my project.'
    Set-ControlText 'Agent prompt' $intro
    $results = [ordered]@{}
    foreach ($test in @(
        @{ kind = '3D models'; file = 'Station fixture.glb'; key = 'model'; method = 'asset.inspect'; extension = 'pmodel' },
        @{ kind = 'Color textures'; file = 'Surface swatch.png'; key = 'color'; method = 'asset.image.inspect'; extension = 'pimage'; color = 'srgb' },
        @{ kind = 'Data textures'; file = 'Surface swatch.png'; key = 'data'; method = 'asset.image.inspect'; extension = 'pimage'; color = 'linear' },
        @{ kind = 'WAV audio'; file = 'Relay audio.wav'; key = 'audio'; method = 'asset.audio.inspect'; extension = 'paudio' }
    )) {
        $file = (Resolve-Path -LiteralPath (Join-Path $AssetFixtures $test.file)).Path
        Select-CreatorAssetFile $test.kind $file
        $draft = Get-ControlText 'Agent prompt'
        $ids = [regex]::Matches($draft, '`([0-9a-f]{64})`')
        if (!$draft.StartsWith($intro) -or $draft.Contains($file) -or $ids.Count -ne ($results.Count + 1)) {
            throw 'The asset handoff replaced the draft, disclosed a local path or omitted the new asset ID.'
        }
        $assetId = $ids[$ids.Count - 1].Groups[1].Value
        $request = @{jsonrpc='2.0'; id=1; method=$test.method; params=@{asset=$assetId}} | ConvertTo-Json -Depth 4 -Compress
        $reply = $request | & $cli connect $endpoint --timeout-ms 2000 | ConvertFrom-Json
        if ($LASTEXITCODE -ne 0 -or $reply.result.asset -cne $assetId -or ($test.color -and $reply.result.color_space -cne $test.color)) {
            throw 'The shared native owner could not inspect the exact imported asset.'
        }
        $stored = Join-Path (Split-Path -Parent $manifest) ('world.json.assets\' + $assetId + '.' + $test.extension)
        if ((Get-FileHash -LiteralPath $stored).Hash.ToLowerInvariant() -cne $assetId) { throw 'Cooked asset identity differs from the draft.' }
        $results[$test.key] = [ordered]@{id=$assetId; bytes=$reply.result.bytes; format=$reply.result.format}
        if ($test.color) { $results[$test.key].color_space = $reply.result.color_space }
        if ($test.key -ceq 'model') { $results.model.triangles = $reply.result.triangles }
    }
    $draftBeforeFailure = Get-ControlText 'Agent prompt'
    Select-CreatorAssetFile '3D models' (Join-Path $AssetFixtures 'Broken.glb') -Reject
    $current = Inspect-Project
    if ((Get-ControlText 'Agent prompt') -cne $draftBeforeFailure -or (Find-Control 'Agent Send' -Enabled) -or
        (Get-ControlText 'Agent transcript') -or $current.world_sha256 -cne $Project.world_sha256 -or
        $current.manifest_sha256 -cne $Project.manifest_sha256 -or $results.color.id -ceq $results.data.id) {
        throw 'One of the imports modified or sent the draft, changed the scene or confused image color spaces.'
    }
    $draftBeforeFailure | Set-Content -LiteralPath (Join-Path $run 'asset-handoff-draft.txt') -Encoding UTF8

    $item = [guid]::NewGuid().ToString('N')
    $request = @{jsonrpc='2.0'; id=1; method='world.transact'; params=@{
        request_id=[guid]::NewGuid().ToString('N'); base_revision=$Project.revision; ops=@(
            @{op='asset.instantiate'; asset=$results.model.id; id=$item; name='Imported station'; parent=$null},
            @{op='component.set'; id=$item; type='Transform'; value=@{position=@(0,1,-3); rotation=@(0,0,0,1); scale=@(1,1,1)}}
        )}} | ConvertTo-Json -Depth 10 -Compress
    $reply = $request | & $cli connect $endpoint --timeout-ms 2000 | ConvertFrom-Json
    $placementExit = $LASTEXITCODE
    $saved = Inspect-Project
    if ($placementExit -ne 0 -or $reply.result.revision -ne ($Project.revision + 1) -or $saved.revision -ne $reply.result.revision -or
        @($saved.assets | Where-Object {$_.sha256 -ceq $results.model.id}).Count -ne 1) {
        throw 'A shared authoring client could not instantiate the asset from the draft.'
    }
    $report.checks.asset_import = [ordered]@{ assets=$results; imported_without_scene_edit=$true; guarded_placement_revision=$saved.revision;
        malformed_source_rejected=$true; draft_and_local_paths_preserved=$true }
    return $saved
}

function Exercise-CodexImportedAsset($Project) {
    if ($CodexExecutable) { Set-AgentExecutable 'Codex executable' $CodexExecutable }
    Invoke-Control 'Agent Connect'
    $null = Wait-For 'the connected asset handoff chat' { Find-Control 'Agent Send' -Enabled } 65
    $draft = Get-ControlText 'Agent prompt'
    $expected = $report.checks.asset_import.assets.model
    Set-ControlText 'Agent prompt' ($draft + "`n`nInspection test only. Ignore gameplay planning and build guides for this turn. " +
        'Call mcp__poima__poima_call exactly once with method asset.inspect, params.asset set to the 64-character ID beside Station fixture.glb above. ' +
        'Read no other files or tools and make no changes. Reply with IMPORTED_TRIANGLES=<actual triangle count> from that response.')
    Invoke-Control 'Agent Send'
    $null = Wait-For 'an asset inspection approval' { Find-Control 'Approve once' -Enabled } 80
    $details = Get-ControlText 'Agent approval details'
    $details | Set-Content -LiteralPath (Join-Path $run 'asset-handoff-approval.txt') -Encoding UTF8
    $lines = $details -split '\r?\n', 2
    if ($lines.Length -ne 2 -or $lines[0] -cne 'poima_call') { throw 'Unexpected tool in the asset handoff.' }
    $operation = $lines[1] | ConvertFrom-Json
    if ($operation.method -cne 'asset.inspect' -or $operation.params.asset -cne $expected.id) {
        Invoke-Control 'Decline'
        throw 'Codex asked to inspect a different asset or tool.'
    }
    Invoke-Control 'Approve once'
    $null = Wait-For 'Codex reporting the cooked triangle count' {
        if (Find-Control 'Agent Send' -Enabled) {
            $text = Get-ControlText 'Agent transcript'
            $text | Set-Content -LiteralPath (Join-Path $run 'asset-handoff-codex.txt') -Encoding UTF8
            if ($text -notmatch ('IMPORTED_TRIANGLES=' + $expected.triangles + '\b')) { throw 'Codex reported the wrong cooked triangle count.' }
            $true
        }
    } 80
    $current = Inspect-Project
    if ($current.world_sha256 -cne $Project.world_sha256 -or $current.manifest_sha256 -cne $Project.manifest_sha256) {
        throw 'The Codex inspection changed the project.'
    }
    $report.checks.asset_import.codex_inspected_id = $expected.id
    $report.checks.asset_import.codex_reported_triangles = $expected.triangles
    Invoke-Control 'Agent Disconnect'
    $null = Wait-For 'the closed asset handoff connection' { Find-Control 'Agent Connect' -Enabled }
}

function Check-ReopenedAssetLibrary {
    $creatorHandle = [IntPtr]$window.Current.NativeWindowHandle
    Invoke-Control ('Import assets' + [char]0x2026)
    $dialog = [IntPtr](Wait-For 'the reopened asset window' {
        $handle = [CreatorTestWindow]::FindDialog($creatorHandle, 'Import assets')
        if ($handle -ne [IntPtr]::Zero) { $handle }
    })
    & {
        $window = [Windows.Automation.AutomationElement]::FromHandle($dialog)
        $row = Wait-For 'the retained model name' { Find-Control 'Station fixture.glb (3D model)' -Enabled }
        $row.GetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern).Select()
        Invoke-Control 'Add to chat draft'
    }
    $id = $report.checks.asset_import.assets.model.id
    $null = Wait-For 'the retained model in the reopened draft' {
        (Get-ControlText 'Agent prompt').Contains($id)
    }
    $report.checks.asset_import.reopened_library_preserved_name_and_id = $true
    $checkpoint = Join-Path $run 'Asset Checkpoint'
    $captured = & $cli project checkpoint $manifest --output $checkpoint | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $captured.status -cne 'ok') { throw 'The labeled project could not be captured.' }
    $modelLabel = Join-Path $checkpoint "source\world.json.assets\$id.pmodel.label"
    if ((Get-Content -LiteralPath $modelLabel -Raw -Encoding UTF8 | ConvertFrom-Json) -cne 'Station fixture.glb') {
        throw 'The editable checkpoint lost the imported model label.'
    }
    $report.checks.asset_import.checkpoint_preserved_name = $true
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

function Verified-PlayRpc($Method, $Params) {
    $request = @{ jsonrpc = '2.0'; id = 1; method = $Method; params = $Params } | ConvertTo-Json -Depth 8 -Compress
    $request | Add-Content -LiteralPath (Join-Path $run 'verified-play-requests.jsonl') -Encoding UTF8
    $raw = $request | & $cli connect $endpoint --timeout-ms 2000
    if ($LASTEXITCODE -ne 0) { throw "The native endpoint failed during $Method." }
    $raw | Add-Content -LiteralPath (Join-Path $run 'verified-play-responses.jsonl') -Encoding UTF8
    $reply = $raw | ConvertFrom-Json
    if ($reply.error -or !$reply.result) { throw "$Method failed: $raw" }
    return $reply.result
}

function Start-VerifiedPlayer($Version) {
    $runtime = Join-Path (Split-Path -Parent $Version.game_manifest) 'runtime\bin\poima.exe'
    Invoke-Control 'Play verified build'
    $script:verifiedPlayer = Wait-For 'the selected standalone player window' {
        Get-Process -Name poima -ErrorAction SilentlyContinue | Where-Object {
            $_.Path -ieq $runtime -and $_.MainWindowHandle -ne [IntPtr]::Zero
        } | Select-Object -First 1
    } 45
    $null = $verifiedPlayer.Handle
    if ((Find-Control 'Play verified build' -Enabled) -or (Find-Control 'Play project' -Enabled)) {
        throw 'A second interactive player was offered while verified playback was active.'
    }
    $null = Wait-For 'status for the running revision' {
        $activity = Find-ControlId 'verified-play-status'
        $activity -and $activity.Current.Name -ceq "Verified revision $($Version.revision) is running in a separate window."
    }
}

function Close-VerifiedPlayer($Version, $Phase) {
    if (![CreatorTestWindow]::PostMessage($verifiedPlayer.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) -or
        !$verifiedPlayer.WaitForExit(20000) -or $verifiedPlayer.ExitCode -ne 0) {
        throw 'The selected standalone player failed to close normally.'
    }
    $stdout = Join-Path $run "$Phase.stdout.txt"
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    while ($deadline.Elapsed.TotalSeconds -lt 15) {
        foreach ($line in (Get-Content -LiteralPath $stdout -Encoding UTF8 | Where-Object { $_.StartsWith('{"event_type":"verified_play_closed"') })) {
            $played = $line | ConvertFrom-Json
            if ($played.bundle_sha256 -cne $Version.bundle_sha256) { continue }
            if ($played.revision -ne $Version.revision -or $played.frames_presented -lt 1 -or
                $played.tick -lt 1 -or $played.stop_reason -cne 'window_closed') {
                throw 'The closed player did not report native presentation for the selected revision.'
            }
            return $played
        }
        Start-Sleep -Milliseconds 100
    }
    throw 'The closed player did not publish a playback receipt.'
}

function Exercise-VerifiedPlayChat($Project, $Passing) {
    $old = $Passing.latest
    Open-CodexQuestionRound $VerifiedPlayChat
    if (!(Find-Control 'Agent Stop' -Enabled) -or (Find-Control 'Play project' -Enabled)) {
        throw 'The test did not hold a turn with the editable preview unavailable.'
    }
    Start-VerifiedPlayer $old
    $updated = Rename-ProjectCamera $Project 'Working on the next version'
    $selectors = @{ profile = 'creator'; project = 'project.poimaproject' }
    $suite = Verified-PlayRpc 'project.verification.read' $selectors
    $job = Verified-PlayRpc 'development.candidate' ($selectors + @{
        expected_suite_sha256 = $suite.sha256; request_id = [guid]::NewGuid().ToString('N')
    })
    $result = Wait-For 'the independent candidate verdict' {
        $current = Verified-PlayRpc 'development.inspect' @{ job_id = $job.job_id; include_output = $false }
        if ($current.state -notin @('queued', 'running')) { $current }
    } 75
    if ($result.state -cne 'succeeded' -or !$result.result.passed) { throw 'The background candidate did not pass.' }
    $retained = Verified-PlayRpc 'project.candidate.retain' ($selectors + @{ job_id = $job.job_id })
    $latest = Read-PlaytestHistory $Project.project_id
    if ($latest.latest.bundle_sha256 -ceq $old.bundle_sha256 -or $latest.latest.revision -ne $updated.revision -or
        $retained.bundle_sha256 -cne $latest.latest.bundle_sha256 -or $verifiedPlayer.HasExited -or
        !(Find-ControlId 'agent-question-1-choice-1')) {
        throw 'The original player, open chat turn or independently retained candidate changed unexpectedly.'
    }
    Answer-CodexQuestionRound
    $null = Wait-For 'the new completed checks shown beside the old player' {
        $work = Find-ControlId 'preview-status'
        $work -and $work.Current.Name -match "^Revision $($updated.revision)\b" -and (Find-Control 'View playtest results' -Enabled)
    } 40
    Inspect-PlaytestDialog $latest $true
    if ($verifiedPlayer.HasExited -or !(Find-Control 'Agent Send' -Enabled)) {
        throw 'The old player or active conversation became unavailable during the refresh.'
    }
    $played = Close-VerifiedPlayer $old 'create'
    $null = Wait-For 'the ready preview controls after natural player exit' { Find-Control 'Play project' -Enabled }
    Invoke-Control 'Agent Disconnect'
    $null = Wait-For 'retirement of the scripted conversation' { Find-Control 'Agent Connect' -Enabled }
    $report.checks.verified_play_chat = [ordered]@{
        old_player = $played; candidate_job_id = $job.job_id; newer_revision = $latest.latest.revision
        newer_bundle_sha256 = $latest.latest.bundle_sha256; newer_report_sha256 = $latest.latest_check.report_sha256
        question_answered_during_play = $true; new_result_opened_during_play = $true; second_player_unavailable = $true
    }
    return $updated
}

function Write-LowJumpOracle($Controller) {
    $height = @{ method = 'runtime.entity'; params = @{ id = $Controller }; path = '/world_matrix/13' }
    $oracle = @{
        format = 'poima.game-verification'; version = 1
        groups = @(@{ name = 'Reduced jump with unchanged landing'; sequence = @(
            @{ step = @{ ticks = 45 } }
            @{ expect = $height + @{ label = 'Starts supported'; between = @(-0.05, 0.05) } }
            @{ step = @{ ticks = 12; inputs = @(@{ entity = $Controller; jump = $true }) } }
            @{ expect = $height + @{ label = 'The jump rises less at tick 57'; between = @(0.30, 0.44) } }
            @{ step = @{ ticks = 70 } }
            @{ expect = $height + @{ label = 'Lands again'; between = @(-0.05, 0.05) } }
        ) })
    }
    $path = Join-Path $run 'independent-low-jump.json'
    [IO.File]::WriteAllText($path, ($oracle | ConvertTo-Json -Depth 12 -Compress))
    return $path
}

function Check-LowJump($Version, $Suite, $Label, [bool]$Pass) {
    $runtime = Join-Path (Split-Path -Parent $Version.game_manifest) 'runtime\bin\poima.exe'
    $path = Join-Path $run "independent-$Label.json"
    $raw = & $runtime game verify $Version.game_manifest --suite $Suite --report $path --completed-exit true `
        --expected-bundle-sha256 $Version.bundle_sha256
    if ($LASTEXITCODE -ne 0) { throw "Independent verification failed to run for $Label." }
    $raw | Set-Content -LiteralPath (Join-Path $run "independent-$Label.stdout.txt") -Encoding UTF8
    $result = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ($result.passed -ne $Pass -or $result.groups.Count -ne 1 -or $result.groups[0].checks.Count -ne 3 -or
        $result.bundle_sha256 -cne $Version.bundle_sha256) {
        throw "The independent jump replay returned the wrong result for $Label."
    }
    return $result
}

function Verify-CodexRetainedLowerJump($Project, $Passing) {
    $old = $Passing.latest
    $oracle = Write-LowJumpOracle $Project.entry.controller
    $oldOracle = Check-LowJump $old $oracle 'original' $false
    $suite = Join-Path (Split-Path -Parent $manifest) 'game.verify.json'
    $suiteHash = (Get-FileHash -LiteralPath $suite).Hash.ToLowerInvariant()
    $worldPath = Join-Path (Split-Path -Parent $manifest) $Project.entry.world
    $before = Get-Content -LiteralPath $worldPath -Raw | ConvertFrom-Json
    $observations = [ordered]@{}
    if ($ObserveVerifiedPlayback) {
        $discovery = Verified-PlayRpc 'world.describe' @{ view = 'catalog' }
        $desktop = Verified-PlayRpc 'desktop.describe' @{}
        if ($discovery.verified_playback_inspection -cne 'desktop.verified.inspect' -or
            !$desktop.methods.'desktop.verified.inspect' -or
            $null -ne (Verified-PlayRpc 'desktop.verified.inspect' @{}).playback) {
            throw 'The creator did not advertise an initially empty verified player observation.'
        }
    }

    Select-AgentProvider 'Codex'
    if ($CodexExecutable) { Set-AgentExecutable 'Codex executable' $CodexExecutable }
    foreach ($controlName in 'Agent allow world edits', 'Agent allow gameplay builds') {
        $control = Wait-For $controlName { Find-Control $controlName -Enabled }
        $toggle = $control.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern)
        if ($toggle.Current.ToggleState -ne 'Off') { throw "Unexpected existing grant for $controlName." }
        $toggle.Toggle()
    }
    Invoke-Control 'Agent Connect'
    $null = Wait-For 'the installed Codex connection' { Find-Control 'Agent Send' -Enabled } 65
    $prompt = ('I approve one change to my already playable first-person room. Lower the player jump speed from 5 to 3 m/s. ' +
        'Use only native Poima MCP tools. Inspect the current controller and revision, use one guarded world.transact component.set, ' +
        'and keep every other field, entity and the existing game.verify.json unchanged. Use the configured creator profile and project.poimaproject. ' +
        'Read the suite hash with project.verification.read. Use development.candidate through poima_build, poll development.inspect until terminal, ' +
        'and use project.candidate.retain through poima_build for the passing job. Keep the old game running and report the new revision.')
    if ($ObserveVerifiedPlayback) {
        $prompt += (' Use desktop.verified.inspect through poima_call before editing and after retaining. Wait until the separately ' +
            'opened verified game has a native presented frame before editing. Use the returned play_id, revision and frame count ' +
            'to tell me whether the earlier verified player stayed open during your work.')
    }
    $turn = [Diagnostics.Stopwatch]::StartNew()
    $transcript = Send-CodexPrompt $prompt {
        $null = Wait-For 'the active agent turn' { Find-Control 'Agent Stop' -Enabled }
        Start-VerifiedPlayer $old
        if ((Inspect-Project).revision -ne $Project.revision) {
            throw 'The agent revised the project before the original game window opened.'
        }
        if ($ObserveVerifiedPlayback) {
            $observations.initial = Wait-For 'native presentation from the original verified player' {
                $sample = (Verified-PlayRpc 'desktop.verified.inspect' @{}).playback
                if ($sample.observation.frames_presented -gt 0) { return $sample }
            } 20
            if ($observations.initial.bundle_sha256 -cne $old.bundle_sha256 -or
                $observations.initial.revision -ne $old.revision -or $observations.initial.state -cne 'running' -or
                $null -eq $observations.initial.observation_age_ms -or $null -eq $observations.initial.observation.focused) {
                throw 'The original observation did not identify the running verified build and its native progress.'
            }
        }
    }
    $turn.Stop()
    $transcript | Set-Content -LiteralPath (Join-Path $run 'verified-play-codex.txt') -Encoding UTF8
    $updated = Inspect-Project
    $latest = Read-PlaytestHistory $Project.project_id
    if ($updated.revision -ne ($Project.revision + 1) -or $latest.latest.revision -ne $updated.revision -or
        !$latest.latest_check.passed -or !$latest.latest_check.retained -or $latest.latest.bundle_sha256 -ceq $old.bundle_sha256 -or
        $latest.latest_check.bundle_sha256 -cne $latest.latest.bundle_sha256 -or $verifiedPlayer.HasExited -or
        (Get-FileHash -LiteralPath $suite).Hash.ToLowerInvariant() -cne $suiteHash) {
        throw 'The model turn did not retain a distinct passing version while the original player stayed open.'
    }
    $after = Get-Content -LiteralPath $worldPath -Raw | ConvertFrom-Json
    $controller = $Project.entry.controller
    $beforeController = $before.entities.PSObject.Properties[$controller].Value.components.CharacterController
    $afterController = $after.entities.PSObject.Properties[$controller].Value.components.CharacterController
    if ($beforeController.jump_speed -ne 5 -or $afterController.jump_speed -ne 3 -or
        $updated.manifest_sha256 -cne $Project.manifest_sha256) { throw 'The authored project did not preserve the intended jump-only edit.' }
    $beforeController.jump_speed = 3
    $afterController.jump_speed = 3
    if (($before.entities | ConvertTo-Json -Depth 50 -Compress) -cne ($after.entities | ConvertTo-Json -Depth 50 -Compress)) {
        throw 'The live model changed an entity or component besides jump speed.'
    }
    $jobs = Verified-PlayRpc 'development.jobs' @{}
    $candidates = @($jobs.jobs | Where-Object { $_.result.bundle_sha256 -ceq $latest.latest.bundle_sha256 })
    if ($candidates.Count -ne 1 -or $candidates[0].state -cne 'succeeded') { throw 'The passing native candidate job was not found.' }
    $candidate = Verified-PlayRpc 'development.inspect' @{ job_id = $candidates[0].job_id; include_output = $false }
    if (!$candidate.result.passed -or $candidate.result.report_sha256 -cne $latest.latest_check.report_sha256 -or
        $candidate.result.suite_sha256 -cne $suiteHash) { throw 'The candidate identity did not match the unchanged suite and retained report.' }
    $newOracle = Check-LowJump $latest.latest $oracle 'revised' $true
    $null = Wait-For 'the retained result while the original game is open' {
        $work = Find-ControlId 'preview-status'
        $work -and $work.Current.Name -match "^Revision $($updated.revision)\b" -and (Find-Control 'View playtest results' -Enabled)
    } 45
    Inspect-PlaytestDialog $latest $true
    if ($ObserveVerifiedPlayback) {
        $observations.after_retention = (Verified-PlayRpc 'desktop.verified.inspect' @{}).playback
        if ($observations.after_retention.play_id -cne $observations.initial.play_id -or
            $observations.after_retention.bundle_sha256 -cne $old.bundle_sha256 -or
            $observations.after_retention.revision -ne $old.revision -or $observations.after_retention.state -cne 'running' -or
            $observations.after_retention.observation.frames_presented -le $observations.initial.observation.frames_presented -or
            (Verified-PlayRpc 'player.inspect' @{}).active) {
            throw 'The running observation changed ownership or build when the newer candidate was retained.'
        }
    }
    $played = Close-VerifiedPlayer $old 'create'
    if ($ObserveVerifiedPlayback) {
        $observations.closed = Wait-For 'retained native closure observation' {
            $last = (Verified-PlayRpc 'desktop.verified.inspect' @{}).playback
            if ($last.state -ceq 'closed') { return $last }
        }
        if ($observations.closed.play_id -cne $observations.initial.play_id -or
            $observations.closed.bundle_sha256 -cne $played.bundle_sha256 -or
            $observations.closed.observation.tick -ne $played.tick -or
            $observations.closed.observation.frames_presented -ne $played.frames_presented -or
            $observations.closed.stop_reason -cne 'window_closed') {
            throw 'The retained verified player observation did not match native closure.'
        }
        $report.checks.verified_playback_observations = $observations
    }
    Invoke-Control 'Agent Disconnect'
    $null = Wait-For 'the retired live Codex connection' { Find-Control 'Agent Connect' -Enabled }
    $report.checks.verified_play_with_codex = [ordered]@{
        old_player = $played; codex_turn_ms = $turn.ElapsedMilliseconds; candidate_job_id = $candidate.job_id
        new_revision = $updated.revision; new_bundle_sha256 = $latest.latest.bundle_sha256
        report_sha256 = $latest.latest_check.report_sha256; unchanged_suite_sha256 = $suiteHash
        independent_suite_sha256 = $newOracle.suite_sha256; independent_original_passed = $oldOracle.passed
        independent_revision_passed = $newOracle.passed; only_jump_speed_changed = $true
    }
    return $updated
}

function Check-VerifiedPlayShutdown($Version) {
    Start-VerifiedPlayer $Version
    $handle = [IntPtr]$window.Current.NativeWindowHandle
    if (![CreatorTestWindow]::PostMessage($handle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)) {
        throw 'Could not ask the creator to close.'
    }
    $null = Wait-For 'the pending creator close' {
        Find-Control 'Close the verified game window to finish closing Poima.'
    }
    # An SDL window handle is available before Vulkan presents its first frame.
    Start-Sleep -Seconds 1
    if ($verifiedPlayer.HasExited) { throw 'Closing the creator unexpectedly stopped the game.' }
    $played = Close-VerifiedPlayer $Version 'verified-close'
    if (!$owner.WaitForExit(20000) -or $owner.ExitCode -ne 0) { throw 'The pending creator close did not finish normally.' }
    $report.checks.verified_play_shutdown = [ordered]@{ player = $played; creator_exit = $owner.ExitCode }
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
$oldReviewMode, $oldReviewTrace = $env:POIMA_TEST_CODEX_REVIEW, $env:POIMA_TEST_CODEX_REVIEW_TRACE
try {
    if ($CodexReviews) {
        $reviewTrace = Join-Path $run 'codex-review-trace'
        New-Item -ItemType Directory -Path $reviewTrace | Out-Null
        $env:POIMA_TEST_CODEX_REVIEW = '1'
        $env:POIMA_TEST_CODEX_REVIEW_TRACE = $reviewTrace
        $report.codex_review_fixture_sha256 = (Get-FileHash -LiteralPath $CodexReviews).Hash.ToLowerInvariant()
    }
    if ($InstallAntigravity) {
        $agyInstallRoot = Join-Path $env:LOCALAPPDATA 'Poima\agents\antigravity'
        if (Test-Path -LiteralPath $agyInstallRoot) { throw 'The per-user Antigravity install folder must be unused.' }
        Get-ChildItem Env: | Where-Object { $_.Name -match '^(GEMINI_|AGY_|GOOGLE_|ANTIGRAVITY_)' } |
            ForEach-Object { Remove-Item ('Env:' + $_.Name) }
    }
    if ($AntigravitySignInPeer) { $env:POIMA_ACP_SIGN_IN_FIXTURE = '1' }
    if ($InstallClaude) {
        $claudeInstallRoot = Join-Path $env:LOCALAPPDATA 'Poima\agents\claude'
        if (Test-Path -LiteralPath $claudeInstallRoot) { throw 'The per-user Claude install folder must be unused.' }
        Get-ChildItem Env: | Where-Object { $_.Name -match '^(ANTHROPIC_|CLAUDE_CODE_|CLAUDE_AGENT_ACP_)' } |
            ForEach-Object { Remove-Item ('Env:' + $_.Name) }
        $env:CLAUDE_CONFIG_DIR = Join-Path $run 'isolated-claude'
        New-Item -ItemType Directory -Path $env:CLAUDE_CONFIG_DIR | Out-Null
    }
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
    if ($AssetFixtures) { $project = Exercise-CreatorAssets $project }
    if ($CodexAssetRead) { Exercise-CodexImportedAsset $project }

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
        if ($VerifiedPlayChat) {
            $project = Exercise-VerifiedPlayChat $project $passing
            $passing = Read-PlaytestHistory $project.project_id
        }
        if ($VerifiedPlayWithCodex) {
            $project = Verify-CodexRetainedLowerJump $project $passing
            $passing = Read-PlaytestHistory $project.project_id
        }
        $project = Edit-ProjectForExport $project 'Playtested viewpoint'
        $failedHistory = Export-PlaytestCandidate $project $false
        if ($failedHistory.latest.bundle_sha256 -cne $passing.latest.bundle_sha256) {
            throw 'The failed attempt replaced the playable version.'
        }
    }
    if ($Antigravity) { Exercise-Acp $project 'Antigravity' 'Antigravity CLI executable' $Antigravity 'antigravity' }
    if ($InstallAntigravity) { Exercise-AntigravityInstall $project }
    if ($AntigravitySignInPeer) { Exercise-AntigravitySignInFixture $AntigravitySignInPeer }
    if ($Claude) { Exercise-Acp $project 'Claude Code' 'Claude ACP executable' $Claude 'claude' }
    if ($InstallClaude) { Exercise-ClaudeInstall $project }
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
    $reviewPeers = @()
    try {
        if ($CodexReviews) {
            $project = Exercise-CodexReviews $project $CodexReviews
            $reviewChecks = $report.checks.codex_reviews
            $reviewPeers = @(Get-Process -Id @($reviewChecks.active_reviewer_before_creator_close, $reviewChecks.lead_pid))
            foreach ($peer in $reviewPeers) { $null = $peer.Handle }
        }
        Close-Owner
        $report.checks.closed_exit_code = $owner.ExitCode
        if ($CodexReviews) {
            $exits = @(Read-ReviewTrace | Where-Object { $_.kind -ceq 'exit' })
            foreach ($peer in $reviewPeers) {
                if (!$peer.HasExited -or $peer.ExitTime -gt $owner.ExitTime -or $peer.Id -notin $exits.pid) {
                    throw "Creator closed before the Codex peer $($peer.Id) exited naturally."
                }
            }
            $reviewChecks.closed_creator_joined_review_and_lead = $true
        }
    }
    finally { foreach ($peer in $reviewPeers) { $peer.Dispose() } }

    $owner = Start-Launcher 'reopen'
    $window = Wait-For 'the reopened launcher' { Find-Window 'Poima' }
    Invoke-Control 'Open selected project'
    $window = Wait-For 'the reopened creator' { Find-Window "$name*Poima Creator" }
    if ($InstallAntigravity) {
        Select-AgentProvider 'Antigravity'
        $settings = Wait-For 'Connection settings after reopening' { Find-Control 'Connection settings' -Enabled }
        $expander = $settings.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
        $expander.Expand()
        $selectedCli = Get-ControlText 'Antigravity CLI executable'
        $expander.Collapse()
        if ($selectedCli -ne (Join-Path $agyInstallRoot '1.5.13\agy.exe')) {
            throw 'Reopened creator did not discover the Poima-managed Antigravity executable.'
        }
        Invoke-Control 'Agent Connect'
        Check-AntigravitySignInChoices $window
        $report.checks.antigravity_install_reopened = $true
    }
    if ($InstallClaude) {
        Check-ClaudeSignIn
        $report.checks.claude_install_reopened = $true
    }
    $reopened = Inspect-Project
    if ($reopened.project_id -ne $project.project_id -or $reopened.revision -ne $project.revision) {
        throw 'Project identity or authoring revision changed after reopen.'
    }
    if ($ObserveVerifiedPlayback) {
        if ($null -ne (Verified-PlayRpc 'desktop.verified.inspect' @{}).playback) {
            throw 'The reopened creator reported playback from a previous owner.'
        }
        $report.checks.verified_observations_owner_scoped = $true
    }
    if ($AssetFixtures) { Check-ReopenedAssetLibrary }
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
    if ($VerifiedPlayChat) {
        $owner = Start-Launcher 'verified-close' $manifest
        $window = Wait-For 'the creator for the verified close check' { Find-Window "$name*Poima Creator" }
        Check-VerifiedPlayShutdown $passing.latest
    }
    if ($WithExport) {
        Play-RelocatedExport $bundle
        if ($AssetFixtures) {
            $relocated = Join-Path $run 'Relocated Windows export'
            $game = Get-Content -LiteralPath (Join-Path $relocated 'game.json') -Raw | ConvertFrom-Json
            $id = $report.checks.asset_import.assets.model.id
            $file = @($game.files | Where-Object { $_.sha256 -ceq $id -and $_.path.EndsWith("$id.pmodel") })
            if ($file.Count -ne 1 -or (Get-FileHash -LiteralPath (Join-Path $relocated $file[0].path)).Hash.ToLowerInvariant() -cne $id) {
                throw 'The relocated game is missing the exact imported model.'
            }
            if ($game.files | Where-Object { $_.path.EndsWith('.label') }) { throw 'The runtime bundle includes an authoring label.' }
            $report.checks.asset_import.relocated_model_verified = $true
        }
    }
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
    if ($CodexReviews -and $owner -and !$owner.HasExited -and $window) {
        try {
            $nodes = $window.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.Condition]::TrueCondition)
            $report.review_failure_controls = @($nodes | Select-Object -First 100 | ForEach-Object {
                @{ label = $_.Current.Name; id = $_.Current.AutomationId; type = $_.Current.ControlType.ProgrammaticName }
            })
        } catch { $report.review_failure_ui_error = $_.ToString() }
    }
    throw
}
finally {
    $env:VK_DRIVER_FILES, $env:VK_ICD_FILENAMES = $oldDriverFiles, $oldIcdFiles
    $env:POIMA_TEST_CODEX_REVIEW, $env:POIMA_TEST_CODEX_REVIEW_TRACE = $oldReviewMode, $oldReviewTrace
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
