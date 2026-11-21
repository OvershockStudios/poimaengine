#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check compiled dialogue through native UI controls, tick batches and saves."""

import argparse
import json
from pathlib import Path
import subprocess
import time

import components_gameplay_contract
import input_actions_gameplay_contract
from components_gameplay_contract import sha, uid
from input_actions_gameplay_contract import OwnedSession, exact, fresh, need


ROOT = Path(__file__).resolve().parents[1]
GAME, IDENTITY = 'Poima.Tests.DialogueProbeGame', 'poima.test.dialogue'
CHOICES = ('begin', 'north', 'ferry', 'lantern', 'unknown', 'stale', 'advance', 'cancel')
CONTROLS = {action: uid(index) for index, action in enumerate(CHOICES, 3)}
TEXT = {
    0: 'The keeper is available.',
    10: 'Keeper: Do you want the north path or the ferry?',
    20: 'Keeper: Take this lantern for the north path.',
    30: 'Keeper: The ferry leaves at dawn.',
    40: 'Keeper: Follow the lanterns along the ridge.',
}
STALE, SAME_TICK, EARLY, CHOICE_REQUIRED, UNKNOWN_CHOICE = 2, 3, 8, 9, 10
STARTED, SELECTED, ENDED, CANCELLED = 11, 13, 14, 15


def scene():
    def ui(number, kind, text, action=None):
        parent = uid(1) if number != 1 else None
        return dict(op='ui.element.set', id=uid(number), element=dict(kind=kind, parent=parent, name=str(number),
                    text=text, action=action, visible=True, enabled=True))

    # All harness buttons stay eligible so guarded commands reach the compiled callback.
    controls = [ui(index, 'button', action.title(), action) for index, action in enumerate(CHOICES, 3)]
    return [dict(op='entity.create', id=uid(100), name='Keeper'), ui(1, 'panel', ''), ui(2, 'label', TEXT[0]), *controls]


def artifact(args):
    files = [Path(__file__), Path(components_gameplay_contract.__file__), Path(input_actions_gameplay_contract.__file__),
             args.binary, *(ROOT/'tests/fixtures/dialogue').glob('*.cs*'), *(ROOT/'packages/dialogue').glob('*.cs*')]
    if args.native_descriptor:
        descriptor = args.native_descriptor.resolve(strict=True)
        need(descriptor.stat().st_size <= 1024 * 1024, 'Native descriptor exceeds 1 MiB')
        spec = json.loads(descriptor.read_bytes())
        need((spec['type'], spec['identity']) == (GAME, IDENTITY), 'Wrong native dialogue fixture descriptor')
        closure = set()
        for entry in spec['files']:
            relative = Path(entry['path'])
            path = (descriptor.parent / relative).resolve(strict=True)
            need(not relative.is_absolute() and '..' not in relative.parts and path.is_relative_to(descriptor.parent) and
                 path.stat().st_size == entry['size'] and sha(path) == entry['sha256'], 'Native artifact closure differs')
            closure.add(path)
        image = (descriptor.parent / spec['library']).resolve(strict=True)
        need(image in closure, 'Native dialogue fixture image is outside its closure')
        config = dict(descriptor=args.native(descriptor), expected_descriptor_sha256=sha(descriptor))
        return image, config, [*files, descriptor, *sorted(closure)]

    image = args.assembly.resolve(strict=True)
    files.extend([image, args.hostfxr, args.bridge, image.parent/'Poima.Dialogue.dll'])
    for assembly in (image, args.bridge):
        files.append(assembly.parent/'Poima.Gameplay.dll')
        files.extend(path for suffix in ('.deps.json', '.runtimeconfig.json')
                     if (path := assembly.with_suffix(suffix)).is_file())
    return image, dict(hostfxr=args.native(args.hostfxr), bridge=args.native(args.bridge),
                      assembly=args.native(image), type=GAME), files


class DialogueFixture:
    def __init__(self, args, root, evidence):
        self.args, self.evidence = args, evidence
        self.world, self.saves = root/'world.json', root/'saves'
        self.saves.mkdir()
        self.owner = None

    def open(self):
        self.owner = OwnedSession(self.args, self.world, self.evidence)
        self.owner.rpc('save.configure', dict(request_id=fresh(), expected_generation=0, root=self.args.native(self.saves)))

    def close(self):
        if self.owner is not None:
            self.owner.close()
            self.owner = None

    def ui(self):
        return self.owner.rpc('runtime.ui.inspect', dict(session_id=self.owner.session, tick=self.owner.tick))

    def guards(self):
        runtime = self.owner.rpc('runtime.inspect', dict(session_id=self.owner.session))
        return dict(session_id=self.owner.session, expected_tick=self.owner.tick,
                    expected_ui_revision=runtime['ui_revision'], expected_control_sequence=runtime['control_sequence'],
                    expected_gameplay_revision=self.owner.game()['revision'])

    def activate(self, action, *, error=False, retry=False):
        request = dict(self.guards(), request_id=fresh(), id=CONTROLS[action])
        result = self.owner.rpc('runtime.ui.activate', request, error=error)
        if not error:
            need(result['current_session_id'] == self.owner.session and result['current_tick'] == self.owner.tick,
                 'Dialogue control unexpectedly advanced the clock or replaced the runtime')
            if retry:
                after = (self.owner.game(), self.ui())
                need(self.owner.rpc('runtime.ui.activate', request) == dict(result, replayed=True) and
                     after == (self.owner.game(), self.ui()), 'Retry reexecuted a dialogue choice')
        return result

    def observe(self, name, tick, node, revision, changed, status):
        values = self.owner.game()['module']['values']
        ui = next(row for row in self.ui()['elements'] if row['id'] == uid(2))
        actual = dict(tick=self.owner.tick, node=values['Node'], revision=values['Revision'],
                      changed=values['ChangedTick'], status=values['LastStatus'], text=ui['text'])
        self.evidence['observations'][name] = actual
        expected = dict(tick=tick, node=node, revision=revision, changed=str(changed), status=status, text=TEXT[node])
        need(exact(actual, expected), f'{name}: expected {expected}, got {actual}')

    def checkpoint(self, slot):
        receipt = self.owner.rpc('save.write', dict(self.guards(), request_id=fresh(), configuration_generation=1,
                                                      slot=slot, expected_generation=0))
        payload, = (self.saves/('slot-'+slot)).glob('p-*.bin')
        need(payload.stat().st_size <= 16*1024*1024 and sha(payload) == receipt['sha256'], 'Durable save receipt differs')
        snapshot = json.loads(payload.read_bytes())['snapshot']
        self.evidence['snapshots'][slot] = dict(receipt=receipt, snapshot=snapshot)
        return snapshot, payload, receipt['sha256']

    def continuation(self, label):
        self.owner.step()
        self.observe(label+'-response-unexpired', 9, 30, 6, 7, SELECTED)
        self.owner.step()
        self.observe(label+'-response-completed', 10, 0, 7, 10, ENDED)
        self.owner.step()
        self.activate('begin')
        self.owner.step()
        self.activate('ferry')
        self.owner.step()
        self.activate('cancel')
        self.observe(label+'-cancelled-before-expiry', 13, 0, 10, 13, CANCELLED)
        self.owner.step(4)
        self.observe(label+'-cancel-persists', 17, 0, 10, 13, CANCELLED)
        return self.checkpoint(label+'-continuation')[0]

    def run(self, image, config):
        self.open()
        revision = self.owner.rpc('world.transact', dict(request_id=fresh(), base_revision=0, ops=scene()))['revision']
        original = self.world.read_bytes()
        self.owner.start(revision)
        module = self.owner.load(config)['module']
        need(module['schema']['identity'] == IDENTITY and module['backend'] == self.evidence['backend'] and
             module['assembly_sha256'] == sha(image), 'Runtime loaded a different compiled dialogue game')

        self.activate('begin', retry=True)
        self.observe('begin', 0, 10, 1, 0, STARTED)
        self.activate('north')
        self.observe('one-transition-per-tick', 0, 10, 1, 0, SAME_TICK)
        self.activate('stale')
        self.observe('stale-dialogue-revision', 0, 10, 1, 0, STALE)
        self.owner.step()
        self.activate('advance')
        self.observe('choice-required', 1, 10, 1, 0, CHOICE_REQUIRED)
        self.activate('unknown')
        self.observe('unknown-choice', 1, 10, 1, 0, UNKNOWN_CHOICE)

        self.owner.edit(FailControl=1)
        before, _, _ = self.checkpoint('control-before')
        error = self.activate('ferry', error=-32040)
        need('Dialogue control fault after selecting node 30.' in error['message'], 'Expected actual staged branch fault')
        after, _, _ = self.checkpoint('control-after')
        need(exact(before, after) and not (self.saves/'slot-dialogue-rejected-intent').exists(),
             'A rejected control published a branch, UI edit or save intent')
        self.owner.edit(FailControl=0)

        self.activate('north', retry=True)
        self.observe('first-branch', 1, 20, 2, 1, SELECTED)
        self.owner.step()
        self.activate('lantern')
        self.observe('only-choice-too-early', 2, 20, 2, 1, EARLY)
        self.owner.step()
        self.activate('lantern')
        self.observe('only-choice-selected', 3, 40, 3, 3, SELECTED)
        self.owner.step()
        self.observe('first-response-unexpired', 4, 40, 3, 3, SELECTED)
        self.owner.edit(FailTick='6')
        before, _, _ = self.checkpoint('tick-before')
        error = self.owner.step(2, error=True)
        need('Dialogue Tick fault after node 0 revision 4.' in error['message'], 'First callback did not finish before second failed')
        after, _, _ = self.checkpoint('tick-after')
        need(exact(before, after), 'Later failure lost the saved dialogue or native UI state')
        self.owner.edit(FailTick='-1')
        self.owner.step()
        self.observe('first-response-ended', 5, 0, 4, 5, ENDED)

        self.owner.step()
        self.activate('begin')
        self.owner.step()
        self.activate('ferry')
        self.observe('alternate-choice-selected', 7, 30, 6, 7, SELECTED)
        self.owner.step()
        self.observe('cold-branch-source', 8, 30, 6, 7, SELECTED)
        saved, payload, digest = self.checkpoint('cold-source')
        expected = self.continuation('original')
        self.owner.rpc('runtime.stop', dict(session_id=self.owner.session))
        self.close()

        self.open()
        session = fresh()
        loaded = self.owner.rpc('save.load', dict(request_id=fresh(), configuration_generation=1, slot='cold-source',
            expected_generation=1, revision=revision, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, new_session_id=session, gameplay=config))
        self.owner.session, self.owner.tick = session, loaded['tick']
        self.observe('cold-branch-restored', 8, 30, 6, 7, SELECTED)
        need(exact(self.checkpoint('cold-restored')[0], saved), 'Fresh process failed to restore the full branch checkpoint')
        need(exact(self.continuation('cold'), expected) and sha(payload) == digest and self.world.read_bytes() == original,
             'Cold progression, original checkpoint or authored source differs')
        self.owner.rpc('runtime.stop', dict(session_id=self.owner.session))
        self.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    for name in ('hostfxr', 'bridge', 'assembly', 'native-descriptor'):
        parser.add_argument('--'+name, type=Path)
    parser.add_argument('--windows-interop', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    development = (args.hostfxr, args.bridge, args.assembly)
    if (args.native_descriptor and any(development)) or (not args.native_descriptor and not all(development)):
        parser.error('Supply --native-descriptor or all three CoreCLR paths')

    def native(path):
        value = str(path.resolve())
        return subprocess.check_output(['wslpath', '-w', value], text=True).strip() if args.windows_interop else value

    args.native, args.deadline = native, time.monotonic()+180
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    evidence = dict(format='poima.dialogue-gameplay-contract.v1', passed=False, owners=[], calls=[], observations={}, snapshots={},
                    backend='native_aot' if args.native_descriptor else 'coreclr',
                    scope='Compiled deterministic dialogue controls, logical UI and durable native checkpoints.')
    fixture = DialogueFixture(args, output, evidence)
    try:
        image, config, paths = artifact(args)
        pins = {str(path.resolve()): sha(path) for path in paths}
        evidence['input_hashes'] = pins
        fixture.run(image, config)
        need(all(sha(path) == digest for path, digest in pins.items()), 'A tested input changed')
        evidence['passed'] = True
    except BaseException as failure:
        evidence['error'] = repr(failure)
        raise
    finally:
        try:
            fixture.close()
        except BaseException as failure:
            evidence['close_error'] = repr(failure)
            if 'error' not in evidence:
                raise
        finally:
            path = output/'evidence.json'
            path.write_text(json.dumps(evidence, indent=2, allow_nan=False)+'\n', encoding='utf-8')
            print(path)


if __name__ == '__main__':
    main()
