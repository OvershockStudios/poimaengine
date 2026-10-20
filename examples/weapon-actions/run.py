#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Explicit interactive native player for an already compiled Weapon Actions game."""
import argparse,json,sys,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient
TYPE='Poima.Examples.WeaponActionsGame'
def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in('binary','project','output'):p.add_argument('--'+name,type=Path,required=True)
    group=p.add_mutually_exclusive_group(required=True);group.add_argument('--assembly',type=Path);group.add_argument('--descriptor',type=Path)
    p.add_argument('--bridge',type=Path);p.add_argument('--hostfxr',type=Path);p.add_argument('--gpu',type=int,default=-1);a=p.parse_args()
    if a.assembly and not(a.bridge and a.hostfxr):p.error('CoreCLR requires bridge and hostfxr.')
    if a.output.exists()or not a.output.parent.is_dir():p.error('Fresh output/existing parent required.')
    manifest=json.loads(a.project.read_bytes());world=a.project.parent/manifest['entry']['world'];a.output.mkdir();calls=[]
    with WorldClient.open(str(a.binary.resolve()),str(world.resolve()),trace_directory=str((a.output/'stdio').resolve()))as client:
        def rpc(method,params=None,timeout=30):
            row=dict(method=method,params=params or{});calls.append(row);row['result']=client.call(method,params or{},timeout=timeout);return row['result']
        try:
            session=uuid.uuid4().hex;revision=rpc('world.inspect')['revision'];rpc('runtime.start',dict(session_id=session,revision=revision))
            state=rpc('runtime.gameplay.inspect',dict(session_id=session,tick=0))
            config=dict(descriptor=str(a.descriptor.resolve()))if a.descriptor else dict(assembly=str(a.assembly.resolve()),bridge=str(a.bridge.resolve()),hostfxr=str(a.hostfxr.resolve()),type=TYPE)
            rpc('runtime.gameplay.load_native'if a.descriptor else'runtime.gameplay.load',dict(session_id=session,request_id=uuid.uuid4().hex,expected_tick=0,expected_revision=state['revision'],**config))
            print('Begin in UI. WASD/mouse move/look; C crouch; right mouse aim; left mouse fire; R reload. Menu pauses. Mode/cancel/holster/equip/reload buttons queue actions until next active tick. Tab releases cursor.',flush=True)
            rpc('runtime.play',dict(request_id=uuid.uuid4().hex,session_id=session,expected_tick=0,controller=manifest['entry']['controller'],camera=manifest['entry']['camera'],mode='interactive',pose_interpolation=True,lens_interpolation=True,width=1280,height=720,gpu=a.gpu),timeout=86400)
        finally:(a.output/'calls.json').write_text(json.dumps(calls,indent=2,allow_nan=False)+'\n')
if __name__=='__main__':main()
