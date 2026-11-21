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
# -WithBuildWorkers adds two editable drafts using the same peer and the native engine.
# -WithLeadWorkers uses the active Codex tool protocol to assign those drafts from main chat.
# -CheckMarkdown inspects formatted lead and worker replies and their original text.
# -WithCombinedDrafts checks selected worker scenes and plays the retained combined export.
# -WithLeadVerification checks tool-driven proposals and approves one after reopening.
# -WithLeadRepair uses failed playtest feedback to revise a draft, verify it and reopen its receipt.
# -WithLongChild checks long Windows bundle paths and native failure observations after approval.
# -WithWorkerReopen closes a working round and checks restored reports, receipts and combined playback.
# -WithWorkerCrash terminates the owned test creator during a worker turn and checks recovery after a draft edit.
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
    [switch]$WithBuildWorkers,
    [switch]$WithLeadWorkers,
    [switch]$CheckMarkdown,
    [switch]$WithCombinedDrafts,
    [switch]$WithLeadVerification,
    [switch]$WithLeadRepair,
    [switch]$WithLongChild,
    [switch]$WithWorkerReopen,
    [switch]$WithWorkerCrash,
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
if ($WithBuildWorkers -and !$CodexReviews) { throw 'Supply -CodexReviews for the build worker scenario.' }
if ($WithLeadWorkers -and (!$CodexReviews -or $WithBuildWorkers)) {
    throw 'Supply -CodexReviews and run lead assignments separately from manual worker assignments.'
}
if ($CheckMarkdown -and !$WithLeadWorkers) { throw 'Run the Markdown checks with -WithLeadWorkers.' }
if (($WithWorkerReopen -or $WithWorkerCrash) -and (!$WithLeadWorkers -or !$WithCombinedDrafts -or $CheckMarkdown -or
    ($WithWorkerReopen -and $WithWorkerCrash))) {
    throw 'Run worker recovery with lead assignments and combined drafts, separately from the Markdown follow-up.'
}
if ($WithCombinedDrafts -and ((!$WithBuildWorkers -and !$WithLeadWorkers) -or $WithoutVulkan)) {
    throw 'Run combined drafts with build workers, a packaged sample runtime and Windows Vulkan.'
}
if ($WithLeadVerification -and (!$WithLeadWorkers -or !$WithCombinedDrafts -or $WithWorkerReopen -or $WithWorkerCrash)) {
    throw 'Run lead verification with lead assignments and combined drafts, as a separate recovery scenario.'
}
if ($WithLeadRepair -and !$WithLeadVerification) { throw 'Run lead repair with -WithLeadVerification.' }
if ($WithLongChild -and !$WithLeadVerification) { throw 'Run the long child test with -WithLeadVerification.' }
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
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool TerminateProcess(IntPtr process, uint exitCode);
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
if ($WithLongChild) {
    # 112 characters here yields a 264-character combined runtime executable path.
    $padding = 112 - $run.Length - 1
    if ($padding -lt 1) { throw 'Use a shorter scratch output for the fixed-depth long child test.' }
    $projects = Join-Path $run ('p' * $padding)
}
$name = 'Creator Workspace'
$manifest = Join-Path (Join-Path $projects $name) 'project.poimaproject'
New-Item -ItemType Directory -Path $projects | Out-Null
$owner = $null
$window = $null
$verifiedPlayer = $null
$combinedEditor = $null
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
    build_workers_requested = [bool]$WithBuildWorkers
    lead_workers_requested = [bool]$WithLeadWorkers
    markdown_requested = [bool]$CheckMarkdown
    combined_drafts_requested = [bool]$WithCombinedDrafts
    lead_verification_requested = [bool]$WithLeadVerification
    lead_repair_requested = [bool]$WithLeadRepair
    long_child_requested = [bool]$WithLongChild
    worker_reopen_requested = [bool]$WithWorkerReopen
    worker_crash_requested = [bool]$WithWorkerCrash
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

function Open-AgentSettings {
    $settings = Wait-For 'Connection settings' { Find-Control 'Connection settings' -Enabled }
    $expander = $settings.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $expander.Expand()
    return $expander
}

function Set-AgentExecutable($Control, $Path, [switch]$AllowMissing) {
    $settings = Open-AgentSettings
    $absolute = if ($AllowMissing) { [System.IO.Path]::GetFullPath($Path) } else { (Resolve-Path -LiteralPath $Path).Path }
    Set-ControlText $Control $absolute
    $settings.Collapse()
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
    $settings = Open-AgentSettings
    foreach ($controlName in 'Agent allow world edits', 'Agent allow source edits', 'Agent allow gameplay builds', 'Agent allow gameplay checks') {
        $control = Find-Control $controlName -Enabled
        if ($control -and $control.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern).Current.ToggleState -ne 'Off') {
            throw "Unexpected blanket authorization for $controlName."
        }
    }
    $settings.Collapse()
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

function Read-BuildWorkerTrace {
    Get-ChildItem -LiteralPath $workerTrace -Filter 'draft-*.jsonl' -File | ForEach-Object {
        Get-Content -LiteralPath $_.FullName -Encoding UTF8 -ErrorAction SilentlyContinue |
            ConvertFrom-Json -ErrorAction SilentlyContinue
    }
}

function Confirm-WorkerExits($Launches) {
    $exits = @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'exit' })
    foreach ($launch in $Launches) {
        if ($launch.thread -notin $exits.thread -or (Get-Process -Id $launch.pid -ErrorAction SilentlyContinue)) {
            throw 'A finished draft still has an open worker process.'
        }
    }
}

function Start-BuildWorkerTask($TaskText) {
    Set-ControlText 'Worker task' $TaskText
    Invoke-Control 'Send to worker'
}

function Wait-ForBuildWorkerReport($Answer) {
    $null = Wait-For "the completed worker report '$Answer'" {
        (Find-ControlId 'worker-status').Current.Name -ceq 'Worker finished' -and
            (Get-ControlText 'Worker report') -ceq $Answer -and (Find-Control 'Add report to main chat' -Enabled)
    } 65
}

function Wait-ForHeldBuildWorker {
    $null = Wait-For 'the pending worker turn' {
        (Find-Control 'Stop worker' -Enabled) -and
            (Get-ControlText 'Worker report') -ceq 'Draft turn is waiting.'
    } 65
}

function Get-AuthoredFileHashes {
    Get-ChildItem -LiteralPath (Split-Path -Parent $manifest) -File -Force -Recurse |
        Where-Object { $_.Extension -ine '.lock' } |
        Sort-Object FullName | ForEach-Object { "$((Get-FileHash -LiteralPath $_.FullName).Hash) $($_.FullName)" }
}

function Write-WorkerSuite($Ids, $Names) {
    $groups = for ($i = 0; $i -lt $Ids.Count; ++$i) {
        @{ name = $Names[$i]; sequence = @(
            @{ step = @{ ticks = 2 } }
            @{ expect = @{
                label = $Names[$i] + ' appears in the game'; method = 'runtime.entity'
                params = @{ id = $Ids[$i] }; path = '/id'; equals = $Ids[$i]
            } }
        ) }
    }
    $suite = @{ format = 'poima.game-verification'; version = 1; groups = @($groups) } | ConvertTo-Json -Depth 10
    [IO.File]::WriteAllText((Join-Path (Split-Path -Parent $manifest) 'game.verify.json'), $suite)
}

function Exercise-BuildWorkers($Project, $LeadProcess) {
    $checks = $report.checks.codex_build_workers = [ordered]@{ base_revision = $Project.revision }
    $ids = @('{0:x32}' -f 0xda01; '{0:x32}' -f 0xda02)
    $names = @('Draft lantern', 'Draft quay bench')
    if ($WithCombinedDrafts) { Write-WorkerSuite $ids $names }
    $before = @(Get-AuthoredFileHashes) -join "`n"
    $originalDraft = 'Keep this build plan.'
    Set-ControlText 'Agent prompt' $originalDraft
    $leadTranscript = Get-ControlText 'Agent transcript'
    $leadTurnCount = @(Read-ReviewTrace | Where-Object { $_.pid -eq $LeadProcess -and $_.kind -ceq 'turn/start' }).Count
    $revision = $Project.revision + 1
    $lanternTask = 'Draft a lantern.'
    $benchTask = 'Draft a quay bench.'
    $holdTask = 'Hold draft turn.'
    $lanternReport = "$($names[0]) created at revision $revision. Entity $($ids[0])."
    $benchReport = "$($names[1]) created at revision $revision. Entity $($ids[1])."

    Invoke-Control 'Add build worker'
    Wait-ForCreatorChatTab 'Worker 1'
    if (Find-Control 'Add report to main chat' -Enabled) { throw 'An empty worker report could be handed off.' }
    Invoke-Control 'Add build worker'
    Wait-ForCreatorChatTab 'Worker 2'
    Wait-ForCreatorChatTab 'Worker 1' -Select
    Start-BuildWorkerTask $lanternTask
    Wait-ForBuildWorkerReport $lanternReport
    Invoke-Control 'Add report to main chat'
    Wait-ForCreatorChatTab 'Main chat'
    Wait-ForCreatorChatTab 'Worker 1' -Select
    Start-BuildWorkerTask $holdTask
    Wait-ForHeldBuildWorker
    Wait-ForCreatorChatTab 'Worker 2' -Select
    Start-BuildWorkerTask $benchTask
    Wait-ForBuildWorkerReport $benchReport
    Invoke-Control 'Add report to main chat'
    Wait-ForCreatorChatTab 'Main chat'

    $leadDraft = Get-ControlText 'Agent prompt'
    if (!$leadDraft.StartsWith($originalDraft, [StringComparison]::Ordinal) -or
        !$leadDraft.Contains($lanternReport) -or !$leadDraft.Contains($benchReport) -or
        (Get-ControlText 'Agent transcript') -cne $leadTranscript -or
        @(Read-ReviewTrace | Where-Object { $_.pid -eq $LeadProcess -and $_.kind -ceq 'turn/start' }).Count -ne $leadTurnCount) {
        throw 'Worker handoff changed the lead chat or lost either report.'
    }
    $leadDraft | Set-Content -LiteralPath (Join-Path $run 'worker-handoff.txt') -Encoding UTF8

    Wait-ForCreatorChatTab 'Worker 1' -Select
    if (!(Find-Control 'Stop worker' -Enabled) -or (Get-ControlText 'Worker report') -cne 'Draft turn is waiting.') {
        throw 'The second worker could not finish independently of the first worker.'
    }
    $records = @(Read-BuildWorkerTrace)
    $assignments = @($lanternTask, $benchTask)
    $manifests = @()
    $markers = @()
    $firstPid = $null
    $secondPid = $null
    for ($i = 0; $i -lt $assignments.Count; ++$i) {
        $task = $assignments[$i]
        $turn = @($records | Where-Object { $_.kind -ceq 'turn/start' -and $_.text.EndsWith($task, [StringComparison]::Ordinal) })
        if ($turn.Count -ne 1) { throw "Expected exactly one worker turn for '$task'." }
        $peer = @($records | Where-Object { $_.pid -eq $turn[0].pid })
        $launch = $peer | Where-Object { $_.kind -ceq 'launch' }
        $thread = $peer | Where-Object { $_.kind -ceq 'thread/start' }
        $catalog = $peer | Where-Object { $_.kind -ceq 'native/catalog' }
        $edit = $peer | Where-Object { $_.kind -ceq 'native/tool' -and $_.request.arguments.method -ceq 'world.transact' }
        if ($launch.command -cne $cli -or ($launch.args -join '|') -cne "mcp|--project|$($launch.manifest)|--draft" -or
            $thread.approvalPolicy -cne 'never' -or $thread.sandbox -cne 'read-only' -or !$thread.ephemeral -or
            $null -eq $thread.environments -or @($thread.environments).Count -ne 0 -or
            !$catalog.native -or (($catalog.tools.PSObject.Properties.Name | Sort-Object) -join '|') -cne
                'poima_call|poima_discover|poima_reference' -or
            $edit.request.arguments.params.base_revision -ne $Project.revision -or
            !$edit.reply.structuredContent.result.committed -or $edit.reply.structuredContent.result.revision -ne $revision) {
            throw "Worker '$task' did not make a native edit through the scoped draft connection."
        }
        $workerRoot = Split-Path -Parent $launch.manifest
        $workerProject = Get-Content -LiteralPath $launch.manifest -Raw -Encoding UTF8 | ConvertFrom-Json
        $marker = Get-Content -LiteralPath (Join-Path $workerRoot '.poima-draft.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        $draftWorld = Get-Content -LiteralPath (Join-Path $workerRoot $workerProject.entry.world) -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($marker.base_project_id -cne $Project.project_id -or $marker.project_id -cne $workerProject.project_id -or
            $marker.project_id -ceq $Project.project_id -or $marker.base_revision -ne $Project.revision -or
            $draftWorld.revision -ne $revision -or
            $draftWorld.entities.PSObject.Properties[$ids[$i]].Value.name -cne $names[$i] -or
            $null -ne $draftWorld.entities.PSObject.Properties[$ids[1 - $i]]) {
            throw "Worker '$task' has incorrect lineage or did not make its own isolated scene edit."
        }
        $manifests += $launch.manifest
        $markers += $marker
        if ($i -eq 0) { $firstPid = $turn[0].pid } else { $secondPid = $turn[0].pid }
    }
    $base = Join-Path (Split-Path -Parent (Split-Path -Parent $manifests[0])) 'base\checkpoint.json'
    $leadPaths = $leadDraft.Replace('\', '/')
    if ($firstPid -eq $secondPid -or $manifests[0] -ceq $manifests[1] -or
        $markers[0].project_id -ceq $markers[1].project_id -or
        $markers[0].base_sha256 -cne $markers[1].base_sha256 -or
        (Get-FileHash -LiteralPath $base).Hash.ToLowerInvariant() -cne $markers[0].base_sha256 -or
        !$leadPaths.Contains($base.Replace('\', '/')) -or !$leadDraft.Contains($markers[0].base_sha256) -or
        !$leadPaths.Contains($manifests[0].Replace('\', '/')) -or !$leadPaths.Contains($manifests[1].Replace('\', '/'))) {
        throw 'The two drafts or the handoff do not identify the same source checkpoint.'
    }
    $checks.base_sha256 = $markers[0].base_sha256
    $checks.draft_project_ids = @($markers[0].project_id, $markers[1].project_id)
    $checks.draft_manifests = $manifests
    $checks.native_edits = $ids
    $checks.independent_concurrent_work = $true
    $checks.handoff_left_draft_unsent = $true

    $finished = @($records | Where-Object { $_.kind -ceq 'launch' -and $_.pid -in @($firstPid, $secondPid) })
    Confirm-WorkerExits $finished
    $checks.finished_workers_retired_before_close = $true
    $held = @($records | Where-Object { $_.kind -ceq 'turn/start' -and $_.text.EndsWith($holdTask, [StringComparison]::Ordinal) })
    if ($held.Count -ne 1 -or $held[0].pid -in @($firstPid,$secondPid) -or
        !$held[0].text.Contains('Previous completed work.') -or !$held[0].text.Contains($lanternReport)) {
        throw 'The next turn did not retain the lantern report in a fresh worker.'
    }
    $held = $held[0]
    $checks.follow_up_inherited_completed_report = $true
    Invoke-Control 'Stop worker'
    $null = Wait-For 'the stopped worker and its clean exit' {
        (Find-ControlId 'worker-status').Current.Name -ceq 'Worker stopped' -and
            !(Get-Process -Id $held.pid -ErrorAction SilentlyContinue) -and
            (Read-BuildWorkerTrace | Where-Object { $_.thread -ceq $held.thread -and $_.kind -ceq 'exit' })
    }
    Wait-ForCreatorChatTab 'Worker 2' -Select
    Wait-ForBuildWorkerReport $benchReport
    $checks.stop_preserved_other_worker_report = $true

    if ($WithCombinedDrafts) {
        Wait-ForCreatorChatTab 'Worker 1' -Select
        Start-BuildWorkerTask 'Check the draft.'
        Wait-ForBuildWorkerReport 'Draft is ready to review.'
        Exercise-CombinedDrafts (Split-Path -Parent (Split-Path -Parent $base)) $ids $Project
    }
    Wait-ForCreatorChatTab 'Worker 1' -Select
    Start-BuildWorkerTask $holdTask
    Wait-ForHeldBuildWorker
    $restarted = Wait-For 'the reopened worker acting in the same draft' {
        $trace = @(Read-BuildWorkerTrace)
        $running = @($trace | Where-Object { $_.kind -ceq 'turn/start' -and $_.thread -cne $held.thread -and
            $_.text.EndsWith($holdTask, [StringComparison]::Ordinal) })
        if ($running.Count -eq 1) {
            $trace | Where-Object { $_.kind -ceq 'launch' -and $_.thread -ceq $running[0].thread -and
                $_.manifest -ceq $manifests[0] }
        }
    }
    Wait-ForCreatorChatTab 'Main chat' -Select
    if ((Get-ControlText 'Agent prompt') -cne $leadDraft -or (Get-ControlText 'Agent transcript') -cne $leadTranscript -or
        (@(Get-AuthoredFileHashes) -join "`n") -cne $before) {
        throw 'Worker conversations changed the live project or the lead chat.'
    }
    $checks.project_unchanged = $true
    $checks.restarted_worker_pid = $restarted.pid
}

function Inspect-MarkdownTranscript($Label) {
    $control = Wait-For $Label { Find-Control $Label }
    $value = $control.GetCurrentPattern([Windows.Automation.ValuePattern]::Pattern).Current
    foreach ($literal in @('## Review notes', '**Lighting**', '```cs', '| Item | State |', '<b>Literal HTML</b>')) {
        if (!$value.IsReadOnly -or !$value.Value.Contains($literal)) { throw "Transcript source changed at $literal." }
    }
    $display = @('Review notes', 'Lighting uses draft.', 'Placement stays separate.', 'var ready = true;',
        'Item', 'Verified', 'Keep the original project.', '<b>Literal HTML</b>')
    foreach ($text in $display) {
        $null = Wait-For "rendered transcript text $text" { Find-Control $text }
    }
    if ((Find-Control '## Review notes') -or (Find-Control '```cs')) {
        throw 'Rendered Markdown exposed syntax markers as transcript blocks.'
    }
}

function Grant-LeadWorkerPermission {
    $settings = Wait-For 'Connection settings' { Find-Control 'Connection settings' }
    $expander = $settings.GetCurrentPattern([Windows.Automation.ExpandCollapsePattern]::Pattern)
    $expander.Expand()
    $setting = Wait-For 'worker assignment permission' { Find-Control 'Allow Codex to assign scene workers' -Enabled }
    $toggle = $setting.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern)
    if ($toggle.Current.ToggleState -ne [Windows.Automation.ToggleState]::Off) { throw 'Scene workers must default to off.' }
    $toggle.Toggle()
    $shell = (Find-Control 'Disable Codex shell access' -Enabled).GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern)
    if ($shell.Current.ToggleState -ne [Windows.Automation.ToggleState]::Off) { throw 'Disabling shell access should be opt-in.' }
    $shell.Toggle()
    $expander.Collapse()
}

function Exercise-LeadWorkers($Project, $Peer) {
    $checks = $report.checks.lead_workers = [ordered]@{ base_revision = $Project.revision }
    $ids = @('{0:x32}' -f 0xda01; '{0:x32}' -f 0xda02)
    $names = @('Draft lantern', 'Draft quay bench')
    if ($WithLeadRepair) { Write-WorkerSuite ($ids + ('{0:x32}' -f 0xda03)) ($names + 'Draft lantern brace') }
    elseif ($WithCombinedDrafts) { Write-WorkerSuite $ids $names }
    $before = @(Get-AuthoredFileHashes) -join "`n"
    Select-AgentProvider 'Codex'
    Set-AgentExecutable 'Codex executable' $Peer
    Invoke-Control 'Agent Connect'
    $null = Wait-For 'the lead with worker permission off' { Find-Control 'Agent Send' -Enabled } 65
    $denied = Send-CodexPrompt "Confirm disabled worker permission at revision $($Project.revision)."
    if (!$denied.Contains('Worker permission is off.') -or (Read-BuildWorkerTrace)) {
        throw 'The initial connection did not deny worker creation.'
    }
    Invoke-Control 'Agent Disconnect'
    $null = Wait-For 'the disconnected lead' { Find-Control 'New chat' -Enabled }
    Grant-LeadWorkerPermission
    Invoke-Control 'New chat'
    $null = Wait-For 'the lead with worker permission granted' { Find-Control 'Agent Send' -Enabled } 65
    $done = Send-CodexPrompt "Delegate scene work at revision $($Project.revision)."
    if (!$done.Contains('Lead collected both worker reports.')) { throw 'The lead did not collect its assigned worker results.' }
    if ($CheckMarkdown) { Inspect-MarkdownTranscript 'Agent transcript' }
    $lead = @(Read-ReviewTrace | Where-Object { $_.kind -ceq 'thread/start' -and $_.dynamicTools.Count -eq 1 })
    $calls = @(Read-ReviewTrace | Where-Object { $_.kind -ceq 'worker/tool' })
    $launches = @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'launch' })
    $edits = @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'native/tool' -and $_.request.arguments.method -ceq 'world.transact' })
    if ($lead.Count -ne 1 -or $lead[0].sandbox -cne 'read-only' -or
        $null -eq $lead[0].environments -or @($lead[0].environments).Count -ne 0 -or
        $launches.Count -ne 2 -or $edits.Count -ne 2 -or
        ($launches.manifest | Select-Object -Unique).Count -ne 2 -or
        ($edits | Where-Object { !$_.reply.structuredContent.result.committed -or
            $_.reply.structuredContent.result.revision -ne $Project.revision + 1 }) -or
        @($calls | Where-Object { !$_.reply.result.success }).Count -ne 5) {
        throw 'The lead did not start exactly two independent native draft owners.'
    }
    for ($i = 1; $i -le 2; ++$i) {
        Wait-ForCreatorChatTab "Worker $i" -Select
        $answer = "$($names[$i-1]) created at revision $($Project.revision + 1). Entity $($ids[$i-1])."
        Wait-ForBuildWorkerReport $answer
    }
    Confirm-WorkerExits $launches
    $checks.finished_workers_retired_before_close = $true
    Wait-ForCreatorChatTab 'Main chat' -Select
    if ($WithCombinedDrafts) {
        $group = Split-Path -Parent (Split-Path -Parent $launches[0].manifest)
        if ($WithLeadVerification) {
            $completed = @($calls | Where-Object { $_.reply.result.success } |
                ForEach-Object { $_.reply.result.contentItems[0].text | ConvertFrom-Json } |
                Where-Object { $_.state -ceq 'completed' })[-1]
            Exercise-LeadVerification $group $completed.request_id $Project.revision
        }
        else { Exercise-CombinedDrafts $group $ids $Project }
    }
    if ($CheckMarkdown) {
        Wait-ForCreatorChatTab 'Worker 1' -Select
        Start-BuildWorkerTask 'Show Markdown report.'
        $null = Wait-For 'the completed Markdown report' { (Find-ControlId 'worker-status').Current.Name -ceq 'Worker finished' }
        Inspect-MarkdownTranscript 'Worker report'
        Wait-ForCreatorChatTab 'Main chat' -Select
        $report.checks.markdown = [ordered]@{ rendered_lead = $true; rendered_worker = $true; source_preserved = $true }
    }
    if ((@(Get-AuthoredFileHashes) -join "`n") -cne $before) { throw 'The lead workers changed the active source project.' }
    $checks.lead_pid = $lead[0].pid
    $checks.worker_manifests = @($launches.manifest)
    $checks.tool_replies = $calls.Count
    $checks.rejected_tool_calls = 5
    $checks.completed_workers = 2
    $checks.project_unchanged = $true
    if ($WithWorkerReopen -or $WithWorkerCrash) {
        $receipt = @($calls | Where-Object { $_.reply.result.success } |
            ForEach-Object { $_.reply.result.contentItems[0].text | ConvertFrom-Json } |
            Where-Object { $_.state -ceq 'completed' })[-1]
        if ((@($receipt.workers.manifest | Sort-Object) -join "`n") -cne
            (@($launches.manifest | Sort-Object) -join "`n")) {
            throw 'The assignment receipt identifies different drafts from the launched workers.'
        }
        $checks.worker_manifests = @($receipt.workers.manifest)
        $checks.assignment_id = $receipt.request_id
        $checks.worker_scene_hashes = @($receipt.workers.world_sha256)
        Invoke-Control 'Add build worker'
        Wait-ForCreatorChatTab 'Worker 3' -Select
        Start-BuildWorkerTask 'Hold draft turn.'
        Wait-ForHeldBuildWorker
        $held = @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'turn/start' -and $_.text.EndsWith('Hold draft turn.') })[0]
        if (!$held) { throw 'The third worker did not start its owned provider turn.' }
        $checks.active_worker_pid = $held.pid
        Wait-ForCreatorChatTab 'Main chat' -Select
    }
}

function Exercise-LeadVerification($Group, $Assignment, $Revision) {
    if ($WithLeadRepair) {
        $reply = Send-CodexPrompt "Repair failed checks for assignment $Assignment."
        if (!$reply.Contains('Lead fixed the failed check and verified all three groups.')) { throw 'The lead did not fix the failed playtest.' }
    }
    else {
        $reply = Send-CodexPrompt "Verify drafts for assignment $Assignment."
        if (!$reply.Contains('Lead checked both combinations.')) { throw 'The lead did not verify the exact requested combinations.' }
    }
    $ledgerPath = Join-Path $Group 'round.json'
    $ledger = Get-Content -LiteralPath $ledgerPath -Raw | ConvertFrom-Json
    $builds = @(Get-ChildItem -LiteralPath $Group -Directory -Filter 'combined-*')
    $candidate = @($ledger.verifications | Where-Object { $_.id -ceq $ledger.proposed })[0]
    $rejected = @($ledger.verifications | Where-Object { $_.result.passed -eq $false })[0]
    $expectedLatest = if ($WithLeadRepair) { $candidate.result.build.bundle } else { $rejected.result.build.bundle }
    if ($builds.Count -ne 2 -or $ledger.verifications.Count -ne 2 -or !$candidate.result.passed -or
        $ledger.latest.bundle -cne $expectedLatest -or $ledger.passing -or (Find-Control 'Play combined game')) {
        throw 'Verification duplicated a build or made the proposal playable before approval.'
    }
    $passingReport = @(Get-ChildItem -File -Path (Join-Path $Group "$($candidate.result.build.folder)\.poima-history\*\*\checks.json"))[0]
    if ((Get-FileHash -LiteralPath $passingReport.FullName).Hash.ToLowerInvariant() -cne $candidate.result.build.report_hash) {
        throw 'The proposed receipt does not identify the written native report.'
    }
    $creator = $window
    Invoke-Control 'Review proposed build'
    try {
        $window = Wait-For 'the proposed build report' { Find-OwnedDialog $creator 'Playtest results' }
        $null = Wait-For 'both proposed check groups' {
            (Find-Control 'Draft lantern (passed)') -and (Find-Control 'Draft quay bench (passed)') -and
                (!$WithLeadRepair -or (Find-Control 'Draft lantern brace (passed)'))
        }
        Invoke-Control 'Keep for later'
    }
    finally { $window = $creator }
    $report.checks.lead_verification = [ordered]@{
        ledger = $ledgerPath
        proposal = $candidate.id
        failed_check = $rejected.id
        passing_report = $passingReport.FullName
        passing_bundle = $candidate.result.build.bundle
        exact_combinations_built = $builds.Count
        reviewed_and_left_pending = $true
    }
    if ($WithLeadRepair) {
        $turns = @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'turn/start' })
        $launches = @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'launch' })
        $edits = @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'native/tool' -and $_.request.arguments.method -ceq 'world.transact' })
        $repair = @($ledger.follow_ups | Where-Object { $_.turn.state -ceq 'completed' })[0]
        $stale = @($ledger.follow_ups | Where-Object { $_.turn.state -ceq 'failed' })[0]
        if ($ledger.follow_ups.Count -ne 2 -or !$repair -or !$stale -or $turns.Count -ne 3 -or
            $launches.Count -ne 3 -or $edits.Count -ne 3) {
            throw 'The exact repair caused additional worker turns, edits or owners.'
        }
        $repairTurn = @($turns | Where-Object { $_.text.EndsWith('Add the lantern brace missing from the playtest.') })[0]
        $originalTurn = @($turns | Where-Object { $_.text.EndsWith('Draft a lantern. Wait for the release signal.') })[0]
        $originalDraft = ($launches | Where-Object { $_.thread -ceq $originalTurn.thread }).manifest
        $repairedDraft = ($launches | Where-Object { $_.thread -ceq $repairTurn.thread }).manifest
        if (!$repairTurn -or !$originalTurn -or $repairTurn.thread -ceq $originalTurn.thread -or
            !$repairTurn.text.Contains('Previous completed work.') -or !$repairTurn.text.Contains('Draft lantern created') -or
            $originalDraft -cne $repairedDraft) {
            throw 'The repair did not carry the prior report into a new connection to the same draft.'
        }
        Confirm-WorkerExits $launches
        Wait-ForCreatorChatTab 'Worker 1' -Select
        $entity = '{0:x32}' -f 0xda03
        Wait-ForBuildWorkerReport "Draft lantern brace created at revision $($Revision + 2). Entity $entity."
        Wait-ForCreatorChatTab 'Main chat' -Select
        $report.checks.lead_repair = [ordered]@{
            request_id = $repair.id
            stale_request_id = $stale.id
            draft_id = $repair.draft_id
            before_sha256 = $repair.expected_world_hash
            after_sha256 = $repair.turn.world_hash
            worker_owners = $launches.Count
            worker_turns = $turns.Count
            committed_world_edits = $edits.Count
        }
    }
}

function Approve-ReopenedProposal {
    $checks = $report.checks.lead_verification
    $creator = $window
    $null = Wait-For 'the restored proposal and failed check' {
        (Find-Control 'Review proposed build' -Enabled) -and ($WithLeadRepair -or (Find-Control 'Combined game checks' -Enabled))
    }
    if (Find-Control 'Play combined game') { throw 'Reopening activated the pending proposal.' }
    $reportBefore = [IO.File]::ReadAllBytes($checks.passing_report)
    Invoke-Control 'Review proposed build'
    try {
        $window = Wait-For 'the reopened proposed build' { Find-OwnedDialog $creator 'Playtest results' }
        $null = Wait-For 'the exact reopened build results' { Find-Control "Tested build $($checks.passing_bundle.Substring(0,12))" }
        [IO.File]::WriteAllBytes($checks.passing_report, [byte[]]($reportBefore + [byte]32))
        Invoke-Control 'Approve this build'
        $window = $creator
        $null = Wait-For 'changed report rejection' {
            (Find-ControlId 'preview-status').Current.Name -match 'proposed build or its playtest report changed'
        }
        if (Find-Control 'Play combined game') { throw 'A modified playtest report activated the pending proposal.' }
    }
    finally {
        [IO.File]::WriteAllBytes($checks.passing_report, $reportBefore)
        $window = $creator
    }
    $checks.changed_report_rejected = $true

    Invoke-Control 'Review proposed build'
    try {
        $window = Wait-For 'the restored exact playtest report' { Find-OwnedDialog $creator 'Playtest results' }
        Invoke-Control 'Approve this build'
    }
    finally { $window = $creator }
    $null = Wait-For 'the approved playable build' { Find-Control 'Play combined game' -Enabled }
    $ledger = Get-Content -LiteralPath $checks.ledger -Raw | ConvertFrom-Json
    if ($ledger.proposed -or $ledger.passing.bundle -cne $checks.passing_bundle -or
        ($ledger.latest.bundle -ceq $ledger.passing.bundle) -ne [bool]$WithLeadRepair) {
        throw 'Approval did not retain the exact proposal alongside the expected latest check.'
    }
    $candidate = Get-Content -LiteralPath $checks.passing_report -Raw | ConvertFrom-Json
    $game = Join-Path (Split-Path -Parent $checks.passing_report) 'game'
    $played = Play-CheckedCombination $game $candidate 'reopen'
    $checks.approved_after_reopen = $true
    $checks.observation = $played.observation
    $checks.playback = $played.playback
    if ($WithLongChild) { Exercise-LongChild $game $candidate $played }
}

function Replay-ReopenedRepair($Peer) {
    $checks = $report.checks.lead_repair
    $earlier = @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'launch' })
    Select-AgentProvider 'Codex'
    Set-AgentExecutable 'Codex executable' $Peer
    Grant-LeadWorkerPermission
    Invoke-Control 'New chat'
    $null = Wait-For 'the reopened lead connected' { Find-Control 'Agent Send' -Enabled } 65
    $reply = Send-CodexPrompt "Replay worker follow-up $($checks.request_id)."
    if (!$reply.Contains('Reopened worker follow-up receipt verified.') -or
        @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'launch' }).Count -ne $earlier.Count) {
        throw 'Replaying the repaired receipt did not preserve the exact turn without starting workers.'
    }
    $checks.replayed_after_reopen_without_new_worker = $true

    $before = @(Get-AuthoredFileHashes) -join "`n"
    $reply = Send-CodexPrompt "Recover blocked scene worker after $($checks.request_id)."
    $records = @(Read-BuildWorkerTrace)
    $peers = @($records | Where-Object { $_.kind -ceq 'launch' -and $_.thread -notin $earlier.thread })
    $unused = @($peers | Where-Object {
        $thread = $_.thread
        $trace = @($records | Where-Object { $_.thread -ceq $thread })
        ($trace.kind -join '|') -ceq 'launch|native/catalog|thread/start|exit'
    })
    $recovery = @($records | Where-Object { $_.kind -ceq 'native/tool' -and
        $_.request.arguments.method -ceq 'world.transact' -and
        $_.request.arguments.params.ops[0].name -ceq 'Worker recovery marker' })
    $recoveryTurn = @($records | Where-Object { $_.kind -ceq 'turn/start' -and
        $_.thread -in $recovery.thread })
    if (!$reply.Contains('Lead recovered the scene worker after its scope was rejected.') -or
        $peers.Count -ne 2 -or $unused.Count -ne 1 -or $recovery.Count -ne 1 -or
        $peers[0].manifest -cne $peers[1].manifest -or $recovery[0].pid -notin $peers.pid -or
        $recoveryTurn.Count -ne 1 -or !$recoveryTurn[0].text.Contains('Draft lantern brace created') -or
        !$recovery[0].reply.structuredContent.result.committed -or
        (@(Get-AuthoredFileHashes) -join "`n") -cne $before) {
        throw 'Creator did not preserve the failed scope receipt and recover the same independent draft.'
    }
    Confirm-WorkerExits $peers
    $checks.worker_scope_recovery = [ordered]@{
        rejected_before_first_turn = $true
        original_hashes_unchanged = $true
        recovered_draft_revision = $recovery[0].reply.structuredContent.result.revision
        original_receipt_preserved = $true
    }
}

function Exercise-ReopenedWorkers($Project, $Peer, $LeadChecks) {
    $sceneChecks = $report.checks.worker_reopen = [ordered]@{}
    $group = Split-Path -Parent (Split-Path -Parent $LeadChecks.worker_manifests[0])
    $ledger = Join-Path $group 'round.json'
    $before = (Get-FileHash -LiteralPath $ledger).Hash.ToLowerInvariant()
    $draftHashes = @($LeadChecks.worker_manifests | ForEach-Object {
        (& $cli project inspect $_ | ConvertFrom-Json).result.world_sha256
    })
    if (($draftHashes -join ',') -cne ($LeadChecks.worker_scene_hashes -join ',')) {
        throw 'The retained drafts differ from the original assignment receipt.'
    }

    $names = @('Draft lantern', 'Draft quay bench')
    $ids = @('{0:x32}' -f 0xda01; '{0:x32}' -f 0xda02)
    for ($i = 1; $i -le 2; ++$i) {
        Wait-ForCreatorChatTab "Worker $i" -Select
        Wait-ForBuildWorkerReport "$($names[$i-1]) created at revision $($Project.revision + 1). Entity $($ids[$i-1])."
    }
    Wait-ForCreatorChatTab 'Worker 3' -Select
    if ((Find-ControlId 'worker-status').Current.Name -cne 'Worker stopped' -or
        (Get-ControlText 'Worker task') -cne 'Hold draft turn.' -or (Find-Control 'Send to worker' -Enabled)) {
        throw 'The stopped worker did not retain its task with the connection off.'
    }
    Wait-ForCreatorChatTab 'Main chat' -Select
    $launches = @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'launch' }).Count
    Select-AgentProvider 'Codex'
    Set-AgentExecutable 'Codex executable' $Peer
    Grant-LeadWorkerPermission
    Invoke-Control 'New chat'
    $null = Wait-For 'the reopened lead connected' { Find-Control 'Agent Send' -Enabled } 65
    $answer = Send-CodexPrompt "Replay worker assignment $($LeadChecks.assignment_id) at revision $($Project.revision)."
    if (!$answer.Contains('Reopened assignment receipt verified.') -or
        @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'launch' }).Count -ne $launches) {
        throw 'Exact replay on reopen launched duplicate workers or lost the receipt.'
    }

    $creator = $window
    Invoke-Control 'Combined game checks'
    $window = Wait-For 'the restored combined results' { Find-OwnedDialog $creator 'Playtest results' }
    $null = Wait-For 'the retained failed check' { Find-Control 'Draft quay bench (failed)' }
    Invoke-Control 'Done'
    $window = $creator
    $combined = $report.checks.combined_drafts
    $candidate = Get-Content -LiteralPath $combined.passing_report -Raw | ConvertFrom-Json
    $runtime = Join-Path (Split-Path -Parent $combined.passing_report) 'game\runtime\bin\poima.exe'
    $beforePlay = Start-TrackedPlayer 'Play combined game' $candidate.bundle_sha256 $runtime
    $tick = Wait-For 'the restored combined bundle presenting' {
        $playing = Read-TrackedPlayback $candidate.bundle_sha256 $beforePlay
        if ($playing.observation.frames_presented -ge 1) {
            ,@($playing.observation.tick)
        }
    } 25
    $sceneChecks.reopened_playback = Close-VerifiedPlayer $candidate 'reopen' $(if ($tick[0] -ge 1) { 1 } else { 0 })

    $pointer = @(Get-ChildItem -File -LiteralPath (Split-Path -Parent $group) -Filter '*.current')[0].FullName
    Invoke-Control 'Start new worker round'
    $null = Wait-For 'the new round selected and previous tabs retired' {
        !(Find-Control 'Worker 1') -and (Find-Control 'Add build worker' -Enabled)
    }
    $newId = [IO.File]::ReadAllText($pointer)
    if ($newId -ceq (Split-Path -Leaf $group) -or (Get-FileHash -LiteralPath $ledger).Hash.ToLowerInvariant() -cne $before -or
        (Find-Control 'Play combined game' -Enabled)) { throw 'Starting a new round lost or reused previous round state.' }
    $sceneChecks.retained_assignment = $LeadChecks.assignment_id
    $sceneChecks.draft_sha256 = $draftHashes
    $sceneChecks.stopped_task_restored = $true
    $sceneChecks.replayed_without_spawning_workers = $true
    $sceneChecks.finished_builds_reopened = $true
    $sceneChecks.old_round_sha256 = $before
    $sceneChecks.new_round_retained_old_files = $true
}

function Stop-OwnedCreatorDuringWorkerTurn($LeadChecks, $Children) {
    $manifest = $LeadChecks.worker_manifests[0]
    $group = Split-Path -Parent (Split-Path -Parent $manifest)
    $ledger = Join-Path $group 'round.json'
    $before = Get-Content -LiteralPath $ledger -Raw | ConvertFrom-Json
    if ($before.workers[2].latest.state -cne 'running' -or $before.workers[2].latest.task -cne 'Hold draft turn.') {
        throw 'The held worker turn was not durably recorded before the crash.'
    }
    $hash = (Get-FileHash -LiteralPath $ledger).Hash.ToLowerInvariant()
    Stop-Process -Id $owner.Id -Force
    if (!$owner.WaitForExit(20000)) { throw 'The terminated test creator did not exit.' }
    foreach ($child in $Children) {
        if (!$child.WaitForExit(20000)) { throw "The dead test creator left its worker peer $($child.Id) running." }
    }
    if ((Get-FileHash -LiteralPath $ledger).Hash.ToLowerInvariant() -cne $hash) {
        throw 'The interrupted creator rewrote the running receipt after termination.'
    }
    $draft = (& $cli project inspect $manifest | ConvertFrom-Json).result
    $request = @{
        jsonrpc='2.0'; id=1; method='world.transact'; params=@{
            request_id=[guid]::NewGuid().ToString('N'); base_revision=$draft.revision
            ops=@(@{op='entity.rename'; id=('{0:x32}' -f 0xda01); name='Changed after the report'})
        }
    } | ConvertTo-Json -Depth 6 -Compress
    $reply = $request | & $cli world --project $manifest | ConvertFrom-Json
    $after = (& $cli project inspect $manifest | ConvertFrom-Json).result
    if ($reply.result.revision -ne $draft.revision + 1 -or $after.revision -ne $reply.result.revision -or
        $after.world_sha256 -ceq $draft.world_sha256) { throw 'The independent draft change failed.' }
    $report.checks.worker_crash = [ordered]@{
        deliberate_termination = $true; child_exit_count = $Children.Count
        running_receipt_sha256 = $hash; edited_world_sha256 = $after.world_sha256
    }
}

function Exercise-CrashRecovery($LeadChecks) {
    $launches = @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'launch' }).Count
    Wait-ForCreatorChatTab 'Worker 3' -Select
    $null = Wait-For 'the interrupted worker status' {
        (Find-ControlId 'worker-status').Current.Name -ceq 'Worker interrupted. Review its draft before sending a follow-up.'
    }
    if ((Get-ControlText 'Worker task') -cne 'Hold draft turn.' -or (Find-Control 'Send to worker' -Enabled)) {
        throw 'Crash recovery did not preserve the task with automatic execution disabled.'
    }
    Wait-ForCreatorChatTab 'Worker 1' -Select
    $null = Wait-For 'the changed draft status' {
        (Find-ControlId 'worker-status').Current.Name -ceq 'Draft changed after its report. Review the current scene in a follow-up.'
    }
    if (Find-Control 'Add report to main chat' -Enabled) { throw 'A stale report could be handed off as current.' }
    $creator = $window
    Invoke-Control 'Review worker scenes'
    $window = Wait-For 'the restored review options' { Find-OwnedDialog $creator 'Review worker scenes' }
    $null = Wait-For 'the unchanged worker eligible for review' { Find-Control 'Include Worker 2' }
    if ((Find-Control 'Include Worker 1') -or (Find-Control 'Include Worker 3')) {
        throw 'A stale or interrupted worker could enter a review.'
    }
    Invoke-Control 'Cancel'
    $window = $creator
    Wait-ForCreatorChatTab 'Main chat' -Select
    $ledger = Join-Path (Split-Path -Parent (Split-Path -Parent $LeadChecks.worker_manifests[0])) 'round.json'
    $after = Get-Content -LiteralPath $ledger -Raw | ConvertFrom-Json
    if ($after.workers[2].latest.state -cne 'interrupted' -or
        $after.workers[0].assigned.world_hash -cne $LeadChecks.worker_scene_hashes[0] -or
        !$after.workers[0].assigned.report -or
        @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'launch' }).Count -ne $launches) {
        throw 'The creator reran interrupted work or replaced the original completed receipt.'
    }
    $checks = $report.checks.worker_crash
    $checks.interrupted_task_restored = $true
    $checks.stale_report_excluded = $true
    $checks.original_receipt_retained = $true
    $checks.workers_not_automatically_restarted = $true
}

function Select-CombinedDrafts([string[]]$Workers) {
    $creator = $window
    Invoke-Control 'Review worker scenes'
    $window = Wait-For 'worker scene review' { Find-OwnedDialog $creator 'Review worker scenes' }
    if (Find-Control 'Check combined game' -Enabled) { throw 'An empty worker selection was accepted.' }
    if (!$Workers) {
        Invoke-Control 'Cancel'
        return
    }
    foreach ($worker in $Workers) {
        $choice = Wait-For "Include $worker" { Find-Control "Include $worker" -Enabled }
        Select-QuestionChoice $choice
    }
    Invoke-Control 'Check combined game'
}

function Exercise-CombinedDrafts($Group, $Ids, $Project) {
    Select-CombinedDrafts @()
    $null = Wait-For 'the cancelled review to close' { Find-Control 'Review worker scenes' -Enabled }
    if ((Get-ChildItem -LiteralPath $Group -Directory -Filter 'combined-*') -or
        (Find-Control 'Play combined game' -Enabled)) { throw 'Cancelled review produced an export.' }

    $attempts = @()
    foreach ($passed in @($true, $false)) {
        $workers = if ($passed) { @('Worker 1', 'Worker 2') } else { @('Worker 1') }
        Select-CombinedDrafts $workers
        $count = if ($passed) { 1 } else { 2 }
        $reports = Wait-For "the completed combined game check $count" {
            if (Find-Control 'Combined game checks' -Enabled) {
                $folders = @(Get-ChildItem -LiteralPath $Group -Directory -Filter 'combined-*')
                $files = @($folders | ForEach-Object {
                    Get-ChildItem -File -Path (Join-Path $_.FullName '.poima-history\*\*\checks.json')
                })
                if ($files.Count -eq $count) { ,$files }
            }
        } 120
        $reportPath = @($reports | Where-Object { $_.FullName -notin $attempts })[0].FullName
        $attempts += $reportPath
        $result = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        if ($result.passed -ne $passed -or $result.groups_passed -ne $workers.Count -or $result.groups_total -ne 2) {
            throw 'The exact selected worker scenes did not produce the expected gameplay checks.'
        }
        $creator = $window
        Invoke-Control 'Combined game checks'
        $dialog = Wait-For 'combined game check results' { Find-OwnedDialog $creator 'Playtest results' }
        $window = $dialog
        try {
            $label = if ($passed) { 'Draft quay bench (passed)' } else { 'Draft quay bench (failed)' }
            $null = Wait-For $label { Find-Control $label }
            Invoke-Control 'Done'
        }
        finally { $window = $creator }
        if ($passed) {
            $candidate = $result
            $game = Join-Path (Split-Path -Parent $reportPath) 'game'
        }
    }

    $played = Play-CheckedCombination $game $candidate 'create'

    Invoke-Control 'Open combined project as copy'
    $copy = Wait-For 'the editable combined project' {
        $folders = @(Get-ChildItem -LiteralPath $projects -Directory -Filter "$name - Combined *")
        if ($folders.Count -eq 1) {
            $path = Join-Path $folders[0].FullName 'project.poimaproject'
            if (Test-Path -LiteralPath $path -PathType Leaf) { $path }
        }
    } 45
    $script:combinedEditor = Wait-For 'the open combined editor' {
        $running = Get-CimInstance Win32_Process -Filter "Name = 'Poima.Editor.exe'" |
            Where-Object { $_.CommandLine -and $_.CommandLine.Contains($copy) } | Select-Object -First 1
        if ($running) {
            $child = Get-Process -Id $running.ProcessId -ErrorAction SilentlyContinue
            if ($child -and $child.MainWindowHandle -ne [IntPtr]::Zero) { $child }
        }
    } 45
    $null = $combinedEditor.Handle
    $opened = (& $cli project inspect $copy | ConvertFrom-Json).result
    $scene = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $copy) $opened.entry.world) -Raw | ConvertFrom-Json
    if ($opened.project_id -in @($Project.project_id, $candidate.project_id) -or $opened.revision -ne $candidate.revision -or
        ($Ids | Where-Object { !$scene.entities.PSObject.Properties[$_] })) { throw 'The editable copy differs from the passing combined scene.' }
    if (![CreatorTestWindow]::PostMessage($combinedEditor.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) -or
        !$combinedEditor.WaitForExit(20000) -or $combinedEditor.ExitCode -ne 0) {
        throw 'The editable copy did not close normally.'
    }
    $report.checks.combined_drafts = [ordered]@{
        cancelled_without_output = $true
        passing_report = $attempts[0]
        failed_report = $attempts[1]
        passing_bundle = $candidate.bundle_sha256
        observation = $played.observation
        playback = $played.playback
        editable_project = $opened.project_id
        editable_close_exit_code = $combinedEditor.ExitCode
    }
}

function Play-CheckedCombination($Game, $Candidate, $Phase) {
    $runtime = Join-Path $Game 'runtime\bin\poima.exe'
    $before = Start-TrackedPlayer 'Play combined game' $Candidate.bundle_sha256 $runtime
    $launched = $verifiedPlayer.Path
    $observation = Wait-For 'native combined playback presentation' {
        $playback = Read-TrackedPlayback $Candidate.bundle_sha256 $before
        if ($playback.observation.frames_presented -ge 1 -and
            ($playback.observation.tick -ge 1 -or (!$playback.observation.focused -and $playback.observation.paused))) {
            $playback.observation
        }
    } 20
    # This automation does not take foreground focus from another application.
    $playback = Close-VerifiedPlayer $Candidate $Phase $(if ($observation.tick -ge 1) { 1 } else { 0 })
    return @{ observation = $observation; playback = $playback; launched_path = $launched }
}

function Read-TrackedPlayback($Bundle, $PreviousPlay, [switch]$AcceptFailure) {
    $sample = (Verified-PlayRpc 'desktop.verified.inspect' @{}).playback
    if (!$sample -or $sample.bundle_sha256 -cne $Bundle -or $sample.play_id -ceq $PreviousPlay) { return }
    if (!$AcceptFailure -and $sample.state -ceq 'failed') {
        $report.checks.native_player_failure = $sample
        throw "The verified player ended during $($sample.failure.stage). $($sample.failure.message)"
    }
    return $sample
}

function Start-TrackedPlayer($Control, $Bundle, $Runtime) {
    $before = (Verified-PlayRpc 'desktop.verified.inspect' @{}).playback.play_id
    Invoke-Control $Control
    $script:verifiedPlayer = Wait-For 'the selected verified player window' {
        $sample = Read-TrackedPlayback $Bundle $before
        if ($sample.process_id) {
            Get-Process -Id $sample.process_id -ErrorAction SilentlyContinue | Where-Object {
                $_.ProcessName -ieq 'poima' -and $_.MainWindowHandle -ne [IntPtr]::Zero
            }
        }
        elseif ($sample.state -ceq 'running') {
            Get-Process -Name poima -ErrorAction SilentlyContinue | Where-Object {
                $_.Path -ieq $Runtime -and $_.MainWindowHandle -ne [IntPtr]::Zero
            } | Select-Object -First 1
        }
    } 45
    $null = $verifiedPlayer.Handle
    return $before
}

function Exercise-LongChild($Game, $Candidate, $Closed) {
    $runtime = Join-Path $Game 'runtime\bin\poima.exe'
    $closedStage = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $Closed.launched_path)))
    if ($runtime.Length -lt 260 -or $Closed.launched_path -ieq $runtime -or $Closed.launched_path.Length -ge 260 -or
        (Test-Path -LiteralPath $closedStage)) {
        throw 'The long-path player did not launch and retire a short-lived copy.'
    }
    $before = Start-TrackedPlayer 'Play combined game' $Candidate.bundle_sha256 $runtime
    $ready = Wait-For 'a presented frame before the injected child exit' {
        $sample = Read-TrackedPlayback $Candidate.bundle_sha256 $before
        if ($sample.observation.frames_presented -ge 1) { $sample }
    } 25
    $process = $verifiedPlayer.Id
    $stage = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $verifiedPlayer.Path)))
    $code = [uint32]3221226505
    if ($ready.process_id -ne $process -or ![CreatorTestWindow]::TerminateProcess($verifiedPlayer.Handle, $code) -or
        !$verifiedPlayer.WaitForExit(20000)) { throw 'Could not inject the expected exit into the owned test player.' }
    $failed = Wait-For 'the structured failure from the same verified player' {
        $sample = Read-TrackedPlayback $Candidate.bundle_sha256 $before -AcceptFailure
        if ($sample.state -ceq 'failed') { $sample }
    }
    if ($failed.process_id -ne $process -or $failed.play_id -cne $ready.play_id -or
        $failed.failure.stage -cne 'result' -or $failed.failure.exit_code -ne $code -or
        $failed.failure.message -cnotmatch '0xC0000409' -or $failed.observation.frames_presented -lt 1 -or
        (Test-Path -LiteralPath $stage)) { throw 'The exact failed child exit or retirement was not observed.' }
    $null = Wait-For 'the saved build remains playable' { Find-Control 'Play combined game' -Enabled }
    $report.checks.long_child = [ordered]@{
        original_executable_characters = $runtime.Length
        staged_executable_characters = $Closed.launched_path.Length
        native_report_in_original_history = $true
        staged_copy_removed_after_close = $true
        injected_process_exit_code = $code
        failure = $failed
        staged_copy_removed_after_failure = $true
    }
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
    $null = Start-TrackedPlayer 'Play verified build' $Version.bundle_sha256 $runtime
    if ((Find-Control 'Play verified build' -Enabled) -or (Find-Control 'Play project' -Enabled)) {
        throw 'A second interactive player was offered while verified playback was active.'
    }
    $null = Wait-For 'status for the running revision' {
        $activity = Find-ControlId 'verified-play-status'
        $activity -and $activity.Current.Name -ceq "Verified revision $($Version.revision) is running in a separate window."
    }
}

function Close-VerifiedPlayer($Version, $Phase, [int]$MinimumTick = 1) {
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
                $played.tick -lt $MinimumTick -or $played.stop_reason -cne 'window_closed') {
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
    $settings = Open-AgentSettings
    foreach ($controlName in 'Agent allow world edits', 'Agent allow gameplay builds') {
        $control = Wait-For $controlName { Find-Control $controlName -Enabled }
        $toggle = $control.GetCurrentPattern([Windows.Automation.TogglePattern]::Pattern)
        if ($toggle.Current.ToggleState -ne 'Off') { throw "Unexpected existing grant for $controlName." }
        $toggle.Toggle()
    }
    $settings.Collapse()
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
$oldWorkerMode, $oldWorkerNative, $oldWorkerTrace =
    $env:POIMA_TEST_CODEX_DRAFT, $env:POIMA_TEST_CODEX_DRAFT_NATIVE, $env:POIMA_TEST_CODEX_DRAFT_TRACE
try {
    if ($CodexReviews) {
        $reviewTrace = Join-Path $run 'codex-review-trace'
        New-Item -ItemType Directory -Path $reviewTrace | Out-Null
        $env:POIMA_TEST_CODEX_REVIEW = '1'
        $env:POIMA_TEST_CODEX_REVIEW_TRACE = $reviewTrace
        $report.codex_review_fixture_sha256 = (Get-FileHash -LiteralPath $CodexReviews).Hash.ToLowerInvariant()
    }
    if ($WithBuildWorkers -or $WithLeadWorkers) {
        $workerTrace = Join-Path $run 'codex-worker-trace'
        New-Item -ItemType Directory -Path $workerTrace | Out-Null
        $env:POIMA_TEST_CODEX_DRAFT = '1'
        $env:POIMA_TEST_CODEX_DRAFT_NATIVE = '1'
        $env:POIMA_TEST_CODEX_DRAFT_TRACE = $workerTrace
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
    $workerPeers = @()
    try {
        if ($CodexReviews -and !$WithLeadWorkers) {
            $project = Exercise-CodexReviews $project $CodexReviews
            $reviewChecks = $report.checks.codex_reviews
            $reviewPeers = @(Get-Process -Id @($reviewChecks.active_reviewer_before_creator_close, $reviewChecks.lead_pid))
            foreach ($peer in $reviewPeers) { $null = $peer.Handle }
        }
        if ($WithBuildWorkers) {
            Exercise-BuildWorkers $project $reviewChecks.lead_pid
            $workerChecks = $report.checks.codex_build_workers
            $workerPeers = @(Get-Process -Id $workerChecks.restarted_worker_pid)
            foreach ($peer in $workerPeers) { $null = $peer.Handle }
        }
        if ($WithLeadWorkers) {
            Exercise-LeadWorkers $project $CodexReviews
            $leadChecks = $report.checks.lead_workers
            $reviewPeers = @(Get-Process -Id $leadChecks.lead_pid)
            if ($leadChecks.active_worker_pid) { $workerPeers = @(Get-Process -Id $leadChecks.active_worker_pid) }
            foreach ($peer in ($reviewPeers + $workerPeers)) { $null = $peer.Handle }
        }
        if ($WithWorkerCrash) { Stop-OwnedCreatorDuringWorkerTurn $leadChecks ($reviewPeers + $workerPeers) }
        else {
            Close-Owner
            $report.checks.closed_exit_code = $owner.ExitCode
            if ($CodexReviews) {
                $exits = @(Read-ReviewTrace | Where-Object { $_.kind -ceq 'exit' })
                foreach ($peer in $reviewPeers) {
                    if (!$peer.HasExited -or $peer.ExitTime -gt $owner.ExitTime -or $peer.Id -notin $exits.pid) {
                        throw "Creator closed before the Codex peer $($peer.Id) exited naturally."
                    }
                }
                if ($WithLeadWorkers) { $leadChecks.closed_creator_joined_lead = $true }
                else { $reviewChecks.closed_creator_joined_review_and_lead = $true }
            }
            if ($WithBuildWorkers -or $WithLeadWorkers) {
                $exits = @(Read-BuildWorkerTrace | Where-Object { $_.kind -ceq 'exit' })
                foreach ($peer in $workerPeers) {
                    if (!$peer.HasExited -or $peer.ExitTime -gt $owner.ExitTime -or $peer.Id -notin $exits.pid) {
                        throw "Creator closed before the build worker $($peer.Id) exited naturally."
                    }
                }
                if ($WithLeadWorkers) { $leadChecks.closed_creator_joined_workers = $true }
                else { $workerChecks.closed_creator_joined_build_workers = $true }
            }
        }
    }
    finally { foreach ($peer in ($reviewPeers + $workerPeers)) { $peer.Dispose() } }

    if ($WithCombinedDrafts) { $owner = Start-Launcher 'reopen' $manifest }
    else {
        $owner = Start-Launcher 'reopen'
        $window = Wait-For 'the reopened launcher' { Find-Window 'Poima' }
        Invoke-Control 'Open selected project'
    }
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
    if ($WithWorkerReopen) { Exercise-ReopenedWorkers $reopened $CodexReviews $report.checks.lead_workers }
    if ($WithWorkerCrash) { Exercise-CrashRecovery $report.checks.lead_workers }
    if ($WithLeadVerification) { Approve-ReopenedProposal }
    if ($WithLeadRepair) { Replay-ReopenedRepair $CodexReviews }
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
    $env:POIMA_TEST_CODEX_DRAFT, $env:POIMA_TEST_CODEX_DRAFT_NATIVE, $env:POIMA_TEST_CODEX_DRAFT_TRACE =
        $oldWorkerMode, $oldWorkerNative, $oldWorkerTrace
    if ($verifiedPlayer -and !$verifiedPlayer.HasExited) {
        $report.forced_player_cleanup = $true
        Stop-Process -Id $verifiedPlayer.Id
    }
    if ($combinedEditor -and !$combinedEditor.HasExited) {
        $null = [CreatorTestWindow]::PostMessage($combinedEditor.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
        if (!$combinedEditor.WaitForExit(10000)) {
            $report.forced_combined_editor_cleanup = $true
            Stop-Process -Id $combinedEditor.Id
        }
    }
    if ($owner -and !$owner.HasExited) {
        $report.forced_cleanup = $true
        Stop-Process -Id $owner.Id
    }
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'report.json') -Encoding UTF8
    Write-Output $run
}
