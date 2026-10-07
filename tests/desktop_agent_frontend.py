#!/usr/bin/env python3
"""Opt-in Windows editor/Codex check using one short model turn and a disposable world."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('editor', 'binary', 'codex', 'output'):
        parser.add_argument('--'+name, required=True, type=Path)
    parser.add_argument('--gpu', type=int, default=1)
    parser.add_argument('--model', help='Explicit model advertised by the selected Codex CLI.')
    args = parser.parse_args()
    if os.name != 'nt': parser.error('Run with Windows Python.')
    root = Path(__file__).resolve().parents[1]
    run = args.output.resolve()/uuid.uuid4().hex
    project = run/'Project'; project.mkdir(parents=True)
    world = project/'world.json'
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    fixture = (root/'examples/interaction-room.jsonl').read_text()
    created = subprocess.run([str(args.binary.resolve()), 'world', str(world)], input=fixture.rstrip()+'\n',
                             text=True, capture_output=True, timeout=20)
    replies = [json.loads(line) for line in created.stdout.splitlines() if line.strip()]
    if created.returncode != 0 or not replies or 'result' not in replies[0] or any('error' in reply for reply in replies):
        raise RuntimeError('Fixture creation failed: '+created.stdout+created.stderr)
    before = json.loads(world.read_text())
    actions = []
    frame = 3
    def act(op, **params):
        nonlocal frame
        actions.append(dict(frame=frame, op=op, **params)); frame += 3
    act('show_agent')
    act('inspect_agent', tag='initial')
    act('set_text', control='Codex executable', text=str(run/'missing-codex.exe'))
    act('click_control', control='Agent Connect')
    act('wait_agent', ready=False, connecting=False, connect_enabled=True, tag='startup_recovered')
    act('set_text', control='Codex executable', text=str(args.codex.resolve()))
    act('set_text', control='Poima executable', text=str(args.binary.resolve()))
    if args.model: act('set_text', control="Model (empty uses the CLI's advertised default)", text=args.model)
    act('click_control', control='Agent Connect')
    act('wait_agent', ready=True, tag='connected')
    act('set_text', control='Agent prompt', text='Use only the Poima MCP tools. Inspect the current world revision, '
        'then create exactly one empty root entity named Agent frontend probe using a guarded world.transact. '
        'Discover the relevant schemas if needed. Do not edit files, run shell commands or change any other entity. '
        'After successful creation reply with exactly POIMA_EDIT_OK.')
    act('click_control', control='Agent Send')
    act('wait_agent', ready=True, status_contains='Turn completed', tag='completed')
    act('inspect_agent', tag='final')
    act('render_agent', path=str(run/'agent.png'), tag='visual')
    act('close_agent')
    act('close_editor')
    script = run/'actions.json'; report = run/'report.json'
    script.write_text(json.dumps(dict(actions=actions), indent=2))
    record = dict(passed=False, editor_sha256=sha(args.editor.parent/'Poima.Editor.dll'),
                  binary_sha256=sha(args.binary), test_sha256=sha(Path(__file__)), gpu=args.gpu,
                  requested_model=args.model,
                  limitations=['Semantic accessible controls; no physical input qualification.',
                               'One Codex conversation and guarded edit; no Claude or general autonomy claim.',
                               'Attached window rendering; no OS screenshot.'])
    try:
        with (run/'stdout.log').open('w') as out, (run/'stderr.log').open('w') as err:
            process = subprocess.Popen([str(args.editor.resolve()), str(world), '--endpoint', 'agent-ui-'+uuid.uuid4().hex,
                '--gpu', str(args.gpu), '--samples', '1', '--layout', str(run/'layout.json'),
                '--script', str(script), '--frames', '6000', '--report', str(report)], stdout=out, stderr=err)
            try: code = process.wait(timeout=240)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=10); raise
        if code != 0: raise RuntimeError('Editor exited with '+str(code)+': '+(run/'stderr.log').read_text())
        result = json.loads(report.read_text())
        if not result.get('success'): raise RuntimeError(result.get('error', 'Editor qualification failed.'))
        if len(result.get('actions', [])) != len(actions): raise RuntimeError('Script did not complete.')
        stages = {spec['tag']: response for spec, response in zip(actions, result['actions']) if 'tag' in spec}
        if stages['initial']['agent']['send_enabled']: raise RuntimeError('Send enabled before connection.')
        final = stages['final']['agent']
        if not final['send_enabled'] or final['stop_enabled'] or final['approvals'] != 0:
            raise RuntimeError('Completed turn left incorrect controls: '+json.dumps(final))
        if 'POIMA_EDIT_OK' not in final['transcript']: raise RuntimeError('Assistant completion missing.')
        after = json.loads(world.read_text())
        added = set(after['entities'])-set(before['entities'])
        if after['revision'] != before['revision']+1 or len(added) != 1:
            raise RuntimeError('Unexpected world changes.')
        entity = after['entities'][next(iter(added))]
        if entity['name'] != 'Agent frontend probe' or entity['parent'] is not None:
            raise RuntimeError('Unexpected created entity.')
        if any(after['entities'].get(k) != v for k, v in before['entities'].items()):
            raise RuntimeError('Existing entities changed.')
        if not (run/'agent.png').is_file(): raise RuntimeError('Agent window render missing.')
        record.update(passed=True, actions=len(actions), final_revision=after['revision'],
                      capture_sha256=sha(run/'agent.png'), world_sha256=sha(world))
    finally:
        (run/'evidence.json').write_text(json.dumps(record, indent=2)+'\n')
        print(run)


if __name__ == '__main__': main()
