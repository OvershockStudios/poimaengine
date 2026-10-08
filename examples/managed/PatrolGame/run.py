#!/usr/bin/env python3
"""Launch the patrol encounter with an owned native world service."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT/'tools'/'python'))
from poima_client import WorldClient


def uid(value): return f'{value:032x}'
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class Encounter:
    """No cached world model, synthesized NPC Inputs, retries or pose edits."""
    def __init__(self, binary, world, config, native=lambda p:str(Path(p).resolve()), timeout=180):
        self.world = Path(world)
        self.config, self.native = config, native
        self.client = WorldClient.open(str(Path(binary).resolve()), native(world))
        self.deadline = time.monotonic()+timeout
        self.session, self.tick = None, 0
        self.calls = 0
        self.closed = False
    def rpc(self, method, params=None, timeout=30):
        remaining = self.deadline-time.monotonic()
        if remaining <= 0: raise TimeoutError('Encounter time budget exhausted')
        result = self.client.call(method, params or {}, timeout=min(timeout,remaining))
        self.calls += 1
        return result
    def author(self):
        request = json.loads((ROOT/'examples'/'patrol-room.jsonl').read_text(encoding='utf-8'))
        return self.rpc(request['method'],request['params'])
    def start(self):
        if self.session: self.rpc('runtime.stop',dict(session_id=self.session))
        self.session, self.tick = uuid.uuid4().hex, 0
        self.rpc('runtime.start',dict(session_id=self.session,revision=self.rpc('world.inspect')['revision']))
        self.step(60)  # Let the two authored capsules settle before the game is initialized.
        self.load()
    def load(self):
        result = self.rpc('runtime.gameplay.load',dict(session_id=self.session,request_id=uuid.uuid4().hex,
            expected_tick=self.tick,expected_revision=self.game()['revision'],**self.config))
        assert result['module']['backend']=='coreclr',result
        return result
    def inspect(self): return self.rpc('runtime.inspect',dict(session_id=self.session))
    def game(self): return self.rpc('runtime.gameplay.inspect',dict(session_id=self.session,tick=self.tick,include_schema=True))
    def values(self): return self.game()['module']['values']
    def entity(self, number):
        return self.rpc('runtime.entity',dict(session_id=self.session,tick=self.tick,id=uid(number)))
    def step(self, ticks=1, move=None, look=None, use=False):
        inputs=[] if move is None and look is None and not use else [dict(entity=uid(100),move=move or [0,0],look=look or [0,0],use=use)]
        result=self.rpc('runtime.step',dict(session_id=self.session,request_id=uuid.uuid4().hex,
            expected_tick=self.tick,expected_structure_revision=self.inspect()['structure_revision'],ticks=ticks,inputs=inputs))
        self.session,self.tick=result['current_session_id'],result['current_tick']
        return result
    def control(self, number):
        state=self.inspect()
        result=self.rpc('runtime.ui.activate',dict(session_id=self.session,request_id=uuid.uuid4().hex,
            expected_tick=self.tick,expected_ui_revision=state['ui_revision'],expected_control_sequence=state['control_sequence'],
            expected_gameplay_revision=self.game()['revision'],expected_structure_revision=state['structure_revision'],id=uid(number)))
        self.session,self.tick=result['current_session_id'],result['current_tick']
        return result
    def close(self):
        if self.closed: return
        self.closed=True
        self.client.close()
        assert self.client.transport.returncode==0, self.client.transport.returncode


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','hostfxr','bridge','assembly','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int,default=-1)
    args=parser.parse_args()
    output=args.output.resolve();output.mkdir(parents=True,exist_ok=False)
    def native(path):
        value=str(Path(path).resolve())
        return subprocess.check_output(['wslpath','-w',value],text=True).strip() if args.windows_interop else value
    config=dict(hostfxr=native(args.hostfxr),bridge=native(args.bridge),assembly=native(args.assembly),type='Poima.Examples.PatrolGame')
    encounter=Encounter(args.binary,output/'world.json',config,native,timeout=86400)
    try:
        encounter.author();encounter.start()
        saves=output/'saves';saves.mkdir()
        encounter.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=native(saves)))
        print('WASD move; mouse look; E makes noise; Tab releases cursor. Alert the guard, break sight, reach the green extraction pad. Relaunch with a new output folder to retry.',flush=True)
        result=encounter.rpc('runtime.play',dict(session_id=encounter.session,request_id=uuid.uuid4().hex,
            expected_tick=encounter.tick,controller=uid(100),camera=uid(101),mode='interactive',width=1280,height=720,gpu=args.gpu),timeout=86400)
        assert result['success'],result
    finally: encounter.close()


if __name__=='__main__':main()
