#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise real compiled handoffs, final-pose placement and whole-save rollback."""
import argparse
import hashlib
import json
from pathlib import Path
import time

import runtime_attachment as fixture
from character_library_intake import multiply
from runtime_pose_frames import id_request
import world_physical_contract as native

GAME = 'Poima.Tests.ItemAttachmentGame'
ITEM, DETAIL, CAMERA = fixture.ITEM, fixture.DETAIL, fixture.CAMERA
FIRST, SECOND = fixture.node(fixture.RIG_A, 1), fixture.node(fixture.RIG_B, 1)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Run:
    def __init__(self, owner, revision, saves, config, native_aot=False):
        self.owner, self.revision, self.saves, self.config = owner, revision, saves, config
        self.native_aot = native_aot
        self.id = id_request()
        self.tick = self.structure = self.game_revision = 0
        owner.rpc('save.configure', dict(request_id=id_request(), expected_generation=0, root=native.native(saves)))

    def start(self, unmarked=False):
        self.owner.rpc('runtime.start', dict(session_id=self.id, revision=self.revision))
        config = self.config if not unmarked else dict(self.config, type='Poima.Tests.ItemAttachmentUnmarkedGame')
        method = 'runtime.gameplay.load_native' if self.native_aot else 'runtime.gameplay.load'
        result = self.call(method, request_id=id_request(), expected_tick=self.tick,
                           expected_structure_revision=self.structure, expected_revision=self.game_revision,
                           values=dict(Item=ITEM, First=FIRST, Second=SECOND), **config)
        self.game_revision = result['revision']

    def call(self, method, error=None, **params):
        return self.owner.rpc(method, dict(session_id=self.id, **params), error)

    def game(self):
        view = self.call('runtime.gameplay.inspect', tick=self.tick, include_schema=True)
        assert view['structure_revision'] == self.structure
        assert view['revision'] == self.game_revision
        return view['module']

    def edit(self, **values):
        result = self.call('runtime.gameplay.edit', request_id=id_request(), expected_tick=self.tick,
                           expected_structure_revision=self.structure, expected_revision=self.game_revision,
                           values=values)
        self.game_revision = result['revision']

    def step(self, ticks=1, changes=0, error=None):
        result = self.call('runtime.step', error, request_id=id_request(), expected_tick=self.tick,
                           expected_structure_revision=self.structure, ticks=ticks)
        if error is None:
            self.tick += ticks
            self.structure += changes
            assert result['tick'] == self.tick, result
            status=self.owner.rpc('runtime.status')
            assert status['tick']==self.tick and status['structure_revision']==self.structure, status
        return result

    def bindings(self):
        return self.call('runtime.attachment.inspect', tick=self.tick, structure_revision=self.structure)['bindings']

    def entity(self, id=ITEM):
        return self.call('runtime.entity', tick=self.tick, structure_revision=self.structure, id=id)

    def matrix(self, id=ITEM):
        return self.entity(id)['world_matrix']

    def bytes(self, slot):
        packages = list((self.saves / f'slot-{slot}').glob('p-*.bin'))
        assert len(packages)==1, packages
        return packages[0].read_bytes()

    def save(self, slot):
        result = self.call('save.write', request_id=id_request(), configuration_generation=1, slot=slot,
                           expected_generation=0, expected_tick=self.tick, expected_gameplay_revision=self.game_revision,
                           expected_structure_revision=self.structure)
        data = self.bytes(slot)
        assert len(data)==result['bytes'] and hashlib.sha256(data).hexdigest()==result['sha256']
        return data

    def restore(self, slot, tick, structure, game_revision):
        result = self.owner.rpc('save.load', dict(request_id=id_request(), configuration_generation=1, slot=slot,
            expected_generation=1, revision=self.revision, expected_session_id=None, expected_tick=None,
            expected_gameplay_revision=None, expected_structure_revision=None, new_session_id=self.id,
            gameplay=self.config))
        assert result['tick']==tick
        self.tick, self.structure, self.game_revision = tick, structure, game_revision

    def capture(self, output, name, gpu):
        if gpu is None:
            return None
        path = output / (name + '.bmp')
        capture = self.call('runtime.capture', tick=self.tick, structure_revision=self.structure,
                            camera=CAMERA, path=native.native(path), width=720, height=405, samples=4, gpu=gpu)
        assert capture['capture_written'] and capture['hardware'] and capture['nvrhi_errors']==0, capture
        return sha(path)


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True)
    native.ARGS, native.DEADLINE = args, time.monotonic()+200
    config = (dict(descriptor=native.native(args.descriptor), expected_descriptor_sha256=sha(args.descriptor))
              if args.descriptor else dict(hostfxr=native.native(args.hostfxr), bridge=native.native(args.bridge),
                                            assembly=native.native(args.assembly), type=GAME))
    report = dict(passed=False, engine_sha256=sha(args.binary), backend='native_aot' if args.descriptor else 'coreclr',
                  source_sha256=sha(Path(__file__).with_name('managed_item_attachment_gameplay')/'Game.cs'),
                  module_sha256=sha(args.descriptor or args.assembly), checks=[], captures={})
    saves = output / 'saves'
    saves.mkdir()
    owner = native.Session(output/'bracket')
    try:
        revision = fixture.configure(owner)
        discovery=owner.rpc('world.describe')['gameplay_item_attachments']
        assert discovery['feature']=='item_attachment_v1' and discovery['services_bytes']==528
        world = owner.world
        authored = world.read_bytes()
        baseline = Run(owner,revision,saves,config,bool(args.descriptor))
        baseline.start()
        baseline.edit(Command=1)
        baseline.step(changes=1)
        assert baseline.bindings()[0]['anchor']==FIRST
        assert baseline.game()['values']['ObservedBinding']==0
        baseline.step(10)
        ready = baseline.save('ready')
        ready_revision=baseline.game_revision
        baseline.step()
        reference = (baseline.matrix(),baseline.matrix(DETAIL))
        report['captures']['uninterrupted_donor']=baseline.capture(output,'uninterrupted-donor',args.gpu)
        owner.close()

        owner = native.Session(output/'handoff',world=world)
        live = Run(owner,revision,saves,config,bool(args.descriptor))
        live.restore('ready',11,1,ready_revision)
        assert live.save('restored-ready')==ready
        live.edit(Command=2)
        receipt=live.step(changes=1)
        assert receipt['save_serviced'] and receipt['save_operation']['generation']==1, receipt
        binding = live.bindings()[0]
        assert binding['anchor']==SECOND
        for id, expected in zip((ITEM,DETAIL),reference):fixture.close_matrix(live.matrix(id),expected)
        view=live.game()['values']
        assert view['ObservedAnchor']==FIRST and view['ObservedTick']=='11' and view['ObservedRevision']=='1', view
        report['captures']['handoff']=live.capture(output,'handoff',args.gpu)
        assert report['captures']['handoff']==report['captures']['uninterrupted_donor']
        switched=live.save('handoff')
        assert live.bytes('handoff-auto')==switched
        handoff_revision=live.game_revision
        live.step(6)
        assert live.game()['values']['ObservedAnchor']==SECOND
        fixture.close_matrix(live.matrix(),multiply(live.matrix(SECOND),binding['offset_matrix']))
        assert live.matrix()!=reference[0]
        advanced=live.save('advanced')
        report['captures']['following']=live.capture(output,'following',args.gpu)
        if args.gpu is not None:assert report['captures']['following']!=report['captures']['handoff']
        report['checks'].append('Live animation advances before transfer. Item and descendant preserve their poses and then follow the receiver.')

        live.edit(Command=6)
        before=live.save('cycle-before')
        error=live.step(error=-32040)
        assert 'cycle' in error['message'].lower(), error
        assert live.save('cycle-after')==before
        advanced_binding=live.bindings()
        live.edit(Command=3,FailAt='19')
        before=live.save('failed-batch-before')
        error=live.step(2,error=-32040)
        assert 'Intentional later tick failure.' in error['message'], error
        assert live.save('failed-batch-after')==before and live.bindings()==advanced_binding
        assert not (saves/'slot-pin-auto').exists()
        report['checks'].append('A graph cycle and a failure after a prior tick staged and committed a pin both preserve complete saved bytes.')

        live.edit(FailAt='-1')
        live.step(changes=1)
        assert live.bytes('pin-auto')==live.save('pin')
        pinned=live.matrix()
        assert live.bindings()[0]['anchor'] is None
        live.step(4)
        fixture.close_matrix(live.matrix(),pinned)
        assert live.game()['values']['ObservedBinding']==1
        live.edit(Command=4)
        live.step(changes=1)
        offset=[1,0,0,0,0,1,0,0,0,0,1,0,.25,.5,-.25,1]
        fixture.close_matrix(live.matrix(),multiply(live.matrix(SECOND),offset))
        live.edit(Command=5)
        live.step(changes=1)
        assert live.bindings()==[]
        assert live.game()['values']['ObservedAnchor']==SECOND
        assert 'attachments' not in json.loads(live.save('reset'))['snapshot']['payload']
        owner.close()
        report['checks'].append('Pinned-world, explicit snapped socket pose and authored reset preserve the defined meaning of all three modes.')

        owner=native.Session(output/'cold',world=world)
        cold=Run(owner,revision,saves,config,bool(args.descriptor))
        cold.restore('handoff',12,2,handoff_revision)
        assert cold.save('cold-handoff')==switched
        cold.step(6)
        assert cold.save('cold-advanced')==advanced
        report['captures']['cold_following']=cold.capture(output,'cold-following',args.gpu)
        assert report['captures']['cold_following']==report['captures']['following']
        owner.close()
        report['checks'].append('Fresh-process restore and continuation reproduce the complete saves at both ticks.')

        if not args.descriptor:
            owner=native.Session(output/'unmarked',world=world)
            unmarked=Run(owner,revision,saves,config)
            unmarked.start(unmarked=True)
            assert 'IItemAttachmentGame' in unmarked.step(error=-32040)['message']
            assert unmarked.bindings()==[]
        assert world.read_bytes()==authored
        report['passed']=True
    except BaseException as failure:
        report['failure']=repr(failure)
        raise
    finally:
        owner.close()
        report.update(owners=native.REPORT['owners'],rpc_count=len(native.REPORT['calls']))
        (output/'report.json').write_text(json.dumps(report,indent=2,allow_nan=False)+'\n')
    print(f'Compiled attachment checks passed in {report["rpc_count"]} native calls.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int)
    parser.add_argument('--hostfxr',type=Path)
    parser.add_argument('--bridge',type=Path)
    parser.add_argument('--assembly',type=Path)
    parser.add_argument('--descriptor',type=Path)
    args=parser.parse_args()
    if bool(args.descriptor)==bool(args.hostfxr and args.bridge and args.assembly):
        parser.error('supply either a Native AOT descriptor or a CoreCLR host, bridge and assembly')
    run(args)


if __name__=='__main__':
    main()
