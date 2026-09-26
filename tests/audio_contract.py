#!/usr/bin/env python3
"""Exercise native audio import, authoring, propagation and captured PCM."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import unittest
import uuid

parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--evidence',type=Path)
parser.add_argument('--hostfxr',type=Path);parser.add_argument('--bridge',type=Path);parser.add_argument('--game',type=Path)
args=parser.parse_args();ROOT=Path(__file__).resolve().parents[1];BINARY=str(args.binary.resolve())
SCRATCH=ROOT/'build/audio-contract'/uuid.uuid4().hex;SCRATCH.mkdir(parents=True)
CAPS=json.loads(subprocess.check_output([BINARY,'capabilities'],text=True))['result']['features']
EVIDENCE=[]
def uid(n):return f'{n:032x}'
def native(p):return subprocess.check_output(['wslpath','-w',str(p.resolve())],text=True).strip() if args.windows_interop else str(p.resolve())
def rpc(method,params):return {'jsonrpc':'2.0','method':method,'params':params}
def create(n,name):return {'op':'entity.create','id':uid(n),'name':name}
def component(n,kind,value):return {'op':'component.set','id':uid(n),'type':kind,'value':value}
def transform(n,pos,rotation=(0,0,0,1),scale=(1,1,1)):return component(n,'Transform',{'position':pos,'rotation':rotation,'scale':scale})
def transaction(revision,ops,preview=False):return rpc('world.transact',{'request_id':uuid.uuid4().hex,'base_revision':revision,'ops':ops,'preview':preview})
def material(transmission=(.5,.15,.03),enabled=True):return {'absorption':[.1,.2,.3],'scattering':.5,'transmission':transmission,'enabled':enabled}
def emitter(asset,gain=1,loop=True,enabled=True):return {'asset':asset,'gain':gain,'loop':loop,'enabled':enabled}
def wave(samples,kind=3,rate=48000,channels=1):
    bits=32 if kind==3 else 16;stride=bits//8*channels
    data=struct.pack('<'+('f' if kind==3 else 'h')*len(samples),*samples)
    fmt=struct.pack('<HHIIHH',kind,channels,rate,rate*stride,stride,bits)
    payload=b'WAVEfmt '+struct.pack('<I',len(fmt))+fmt+b'data'+struct.pack('<I',len(data))+data
    return b'RIFF'+struct.pack('<I',len(payload))+payload
def pcm(path):
    b=path.read_bytes();assert b[:4]==b'RIFF' and struct.unpack_from('<I',b,4)[0]==len(b)-8
    parts={};p=12
    while p<len(b):
        n=struct.unpack_from('<I',b,p+4)[0];parts[b[p:p+4]]=b[p+8:p+8+n];p+=8+n+(n&1)
    assert struct.unpack_from('<HHI',parts[b'fmt '])==(3,2,48000)
    values=struct.unpack('<'+'f'*(len(parts[b'data'])//4),parts[b'data']);assert all(math.isfinite(v) for v in values)
    assert struct.unpack('<I',parts[b'fact'])[0]==len(values)//2
    return values
def rms(samples):return math.sqrt(sum(x*x for x in samples)/len(samples))

class AudioContract(unittest.TestCase):
    def setUp(self):
        self.directory=SCRATCH/self._testMethodName;self.directory.mkdir();self.world=self.directory/'world.json'
    def requests(self,requests):
        values=[{**r,'id':i+1} for i,r in enumerate(requests)]
        p=subprocess.run([BINARY,'world',native(self.world)],input=''.join(json.dumps(r)+'\n' for r in values),capture_output=True,text=True,timeout=120)
        self.assertEqual(p.returncode,0,p.stdout+p.stderr);replies=[json.loads(line) for line in p.stdout.splitlines()];self.assertEqual(len(replies),len(values),p.stdout+p.stderr)
        self.assertEqual([r['id'] for r in replies],list(range(1,len(replies)+1)))
        EVIDENCE.append({'test':self._testMethodName,'requests':values,'responses':replies,'stderr':p.stderr})
        return replies
    def result(self,r):self.assertIn('result',r,r);return r['result']
    def error(self,r,code):self.assertEqual(r['error']['code'],code,r)
    def import_clip(self,samples=None):
        if samples is None:samples=[.12*sum(math.sin(2*math.pi*f*i/48000) for f in [200,1000,6000]) for i in range(4800)]
        source=self.directory/'original.wav';source.write_bytes(wave(samples));info=self.result(self.requests([rpc('asset.audio.import',{'source':native(source)})])[0]);self.asset=info['asset'];return info
    def room(self,door=True,source=(0,1.5,-6),listener=(0,1.5,2),gain=1,loop=True):
        ops=[create(100,'Listener'),transform(100,listener),create(101,'Source'),transform(101,source),component(101,'AudioEmitter',emitter(self.asset,gain,loop))]
        if door:ops += [create(2,'Door'),transform(2,[0,1.5,-3],scale=[2,3,.4]),component(2,'BoxCollider',{'half_extents':[.5,.5,.5],'motion':'kinematic','mass':1,'friction':.5,'restitution':0}),component(2,'AcousticMaterial',material())]
        return transaction(0,ops)
    def inspect(self,rev=1,live=False,tick=0):return rpc('runtime.audio.inspect' if live else 'world.audio.inspect',{'session_id':uid(900),'tick':tick,'listener':uid(100)} if live else {'revision':rev,'listener':uid(100)})
    def capture(self,name,rev=1,frames=24000,live=False,tick=0):
        r=self.inspect(rev,live,tick);r['method']=r['method'].replace('inspect','capture');r['params'].update({'path':native(self.directory/(name+'.wav')),'frames':frames});return r

    def test_clip_import_is_source_independent_and_hash_checked(self):
        info=self.import_clip([-.5,0,.5]);self.assertEqual(info['frames'],3);self.assertEqual(info['channels'],1)
        source=self.directory/'original.wav';source.unlink()
        r=self.requests([rpc('asset.audio.inspect',{'asset':self.asset}),self.room(False)]);self.assertEqual(self.result(r[0]),info);self.result(r[1])
        asset=Path(str(self.world)+'.assets')/(self.asset+'.paudio')
        # Locate by authoritative content hash, independent of the store suffix.
        if not asset.exists():asset=next(self.directory.rglob(self.asset+'.paudio'))
        original=asset.read_bytes();self.assertEqual(hashlib.sha256(original).hexdigest(),self.asset)
        asset.write_bytes(original[:-1]+bytes([original[-1]^1]));r=self.requests([rpc('asset.audio.inspect',{'asset':self.asset})]);self.error(r[0],-32050)

    def test_wav_validation_and_pcm16(self):
        good=self.directory/'pcm16.wav';good.write_bytes(wave([-32768,0,32767],kind=1))
        r=self.requests([rpc('asset.audio.import',{'source':native(good)})]);self.assertEqual(self.result(r[0])['frames'],3)
        malformed=[wave([float('nan')]),wave([float('inf')]),wave([1.01]),wave([0],rate=44100),wave([0,0],channels=2),wave([]),wave([0])[:-1],b'not a wave']
        requests=[]
        for i,b in enumerate(malformed):
            path=self.directory/f'bad-{i}.wav';path.write_bytes(b);requests.append(rpc('asset.audio.import',{'source':native(path)}))
        for r in self.requests(requests):self.error(r,-32050)

    def test_authoring_schema_preview_and_validation(self):
        self.import_clip();r=self.requests([rpc('world.describe',{}),self.room(False),transaction(1,[component(101,'AudioEmitter',emitter(self.asset,.25))],True),rpc('entity.get',{'id':uid(101),'component':'AudioEmitter'}),transaction(1,[component(100,'AcousticMaterial',material())]),transaction(1,[component(101,'AudioEmitter',emitter(self.asset,5))])])
        self.assertIn('world.audio.capture',self.result(r[0])['methods']);self.result(r[1]);self.assertFalse(self.result(r[2])['committed']);self.assertEqual(self.result(r[3])['value']['gain'],1);self.error(r[4],-32602);self.error(r[5],-32602)
        if not CAPS['audio_capture']:self.error(self.requests([self.inspect()])[0],-32003)

    @unittest.skipUnless(CAPS['audio_capture'],'Optional Steam Audio not built')
    def test_stationary_endpoints_door_opening_and_material_filter(self):
        self.import_clip()
        r=self.requests([self.room(),self.capture('closed'),transaction(1,[transform(2,[2.2,1.5,-3],scale=[2,3,.4])]),self.capture('open',2),self.capture('repeat',2),transaction(2,[component(101,'AudioEmitter',emitter(self.asset,.25))]),self.capture('quiet',3),self.inspect(1)])
        self.result(r[0]);closed=self.result(r[1]);opened=self.result(r[3]);self.result(r[4]);self.result(r[6]);self.error(r[7],-32009)
        a,b=closed['sources'][0],opened['sources'][0];self.assertEqual(a['source_position'],b['source_position']);self.assertEqual(a['listener_position'],b['listener_position']);self.assertEqual(a['direct_visibility'],0);self.assertEqual(b['direct_visibility'],1)
        self.assertLess(a['transmission'][2],a['transmission'][0]);self.assertEqual(closed['triangles'],12)
        blocked=pcm(self.directory/'closed.wav');clear=pcm(self.directory/'open.wav');quiet=pcm(self.directory/'quiet.wav')
        self.assertGreater(rms(clear[4096:]),rms(blocked[4096:])*2);self.assertEqual(clear,pcm(self.directory/'repeat.wav'));self.assertAlmostEqual(rms(quiet)/rms(clear),.25,places=5)
        def band(values,f):
            # Integer periods after filter startup; independent frequency measurement.
            left=values[2*4800:2*14400:2];return abs(sum(x*complex(math.cos(2*math.pi*f*i/48000),math.sin(2*math.pi*f*i/48000)) for i,x in enumerate(left)))
        low=band(blocked,200)/band(clear,200);high=band(blocked,6000)/band(clear,6000);self.assertLess(high,low*.5)

    @unittest.skipUnless(CAPS['audio_capture'] and CAPS['simulation'],'Audio and simulation required')
    def test_runtime_geometry_is_current_and_authoring_is_frozen(self):
        self.import_clip();session={'session_id':uid(900)}
        r=self.requests([self.room(),rpc('runtime.start',{**session,'revision':1}),self.capture('runtime-closed',live=True),transaction(1,[component(2,'AcousticMaterial',material(enabled=False))]),self.inspect(live=True),rpc('runtime.step',{**session,'request_id':uid(1000),'expected_tick':0,'ticks':120,'motions':[{'entity':uid(2),'position':[2.2,1.5,-3],'rotation':[0,0,0,1],'duration_ticks':120}]}),self.capture('runtime-open',live=True,tick=120),rpc('runtime.inspect',session),self.inspect(live=True,tick=0),self.inspect(2)])
        for i in [0,1,2,3,4,5,6,7,9]:self.result(r[i])
        self.assertEqual(self.result(r[4])['sources'][0]['direct_visibility'],0);self.assertEqual(self.result(r[6])['sources'][0]['direct_visibility'],1)
        self.assertEqual(self.result(r[6])['revision'],1);self.assertEqual(self.result(r[7])['tick'],120);self.error(r[8],-32009);self.assertEqual(self.result(r[9])['triangles'],0)
        self.assertGreater(rms(pcm(self.directory/'runtime-open.wav')),rms(pcm(self.directory/'runtime-closed.wav'))*2)

    @unittest.skipUnless(CAPS['audio_capture'],'Optional Steam Audio not built')
    def test_distance_delay_elevation_and_listener_orientation(self):
        self.import_clip([.5]+[0]*255)
        r=self.requests([self.room(False,source=(0,0,-343),listener=(0,0,0),loop=False),self.capture('distant',frames=60000),transaction(1,[transform(101,[2,2,-2])]),self.capture('above',2,frames=4096),transaction(2,[transform(101,[2,-2,-2])]),self.capture('below',3,frames=4096),transaction(3,[transform(101,[-2,-2,-2])]),self.capture('left',4,frames=4096),transaction(4,[transform(100,[0,0,0],rotation=[0,1,0,0])]),self.capture('turned',5,frames=4096)])
        for item in r:self.result(item)
        self.assertEqual(self.result(r[1])['sources'][0]['propagation_delay_samples'],48000);distant=pcm(self.directory/'distant.wav');self.assertTrue(all(v==0 for v in distant[:96000]));self.assertGreater(rms(distant[96000:]),1e-7)
        above=pcm(self.directory/'above.wav');below=pcm(self.directory/'below.wav');left=pcm(self.directory/'left.wav');turned=pcm(self.directory/'turned.wav')
        self.assertNotEqual(above,below);self.assertGreater(rms(below[1::2]),rms(below[::2]));self.assertGreater(rms(left[::2]),rms(left[1::2]));self.assertGreater(rms(turned[1::2]),rms(turned[::2]))

    @unittest.skipUnless(CAPS['audio_capture'],'Optional Steam Audio not built')
    def test_audio_observation_ignores_unrelated_graphics_assets(self):
        self.import_clip();room=self.room(False);room['params']['ops'] += [create(200,'Uncooked visual placeholder'),component(200,'StaticMesh',{'asset':'a'*64,'primitive':0,'visible':True}),component(200,'PbrTextures',{'base_color':{'asset':'b'*64}})]
        r=self.requests([room,self.capture('without-render-assets')])
        for item in r:self.result(item)
        self.assertEqual(self.result(r[1])['triangles'],0);self.assertGreater(rms(pcm(self.directory/'without-render-assets.wav')),0)

    @unittest.skipUnless(CAPS['audio_capture'] and CAPS['managed_gameplay'] and args.hostfxr and args.bridge and args.game,'Explicit CoreCLR bridge/game paths required')
    def test_CSharp_use_changes_native_audio_path(self):
        self.import_clip();fixture=json.loads((ROOT/'examples/interaction-room.jsonl').read_text());session={'session_id':uid(900)}
        sound=transaction(1,[component(2,'AcousticMaterial',material()),create(300,'Sound beyond the door'),transform(300,[0,1.5,-6]),component(300,'AudioEmitter',emitter(self.asset))])
        config={**session,'request_id':uid(4000),'expected_tick':0,'expected_revision':0,'hostfxr':native(args.hostfxr),'bridge':native(args.bridge),'assembly':native(args.game),'type':'Poima.Examples.DoorGame','values':{'UseDistance':10}}
        def step(tick,use=False):return rpc('runtime.step',{**session,'request_id':uuid.uuid4().hex,'expected_tick':tick,'ticks':120,'inputs':[{'entity':uid(100),'use':use}]})
        def capture(name,tick):return rpc('runtime.audio.capture',{**session,'tick':tick,'listener':uid(101),'frames':24000,'path':native(self.directory/(name+'.wav'))})
        r=self.requests([fixture,sound,rpc('runtime.start',{**session,'revision':2}),rpc('runtime.gameplay.load',config),step(0),capture('CSharp-closed',120),step(120,True),capture('CSharp-open',240),rpc('runtime.gameplay.inspect',session)])
        for item in r:self.result(item)
        closed=self.result(r[5])['sources'][0];opened=self.result(r[7])['sources'][0]
        self.assertEqual(closed['source_position'],opened['source_position']);self.assertEqual(closed['listener_position'],opened['listener_position']);self.assertEqual(closed['direct_visibility'],0);self.assertEqual(opened['direct_visibility'],1)
        self.assertEqual(self.result(r[8])['module']['values']['Activations'],1)
        self.assertGreater(rms(pcm(self.directory/'CSharp-open.wav')),rms(pcm(self.directory/'CSharp-closed.wav'))*2)

    @unittest.skipUnless(CAPS['audio_capture'],'Optional Steam Audio not built')
    def test_imported_mesh_geometry_and_scaled_source(self):
        from gltf_fixture import sphere,glb
        self.import_clip();path=self.directory/'sphere.glb';path.write_bytes(glb(*sphere(6,12)))
        model=self.result(self.requests([rpc('asset.import',{'source':native(path)})])[0])['asset']
        room=self.room(False);room['params']['ops'] += [create(2,'Acoustic imported mesh'),transform(2,[0,1.5,-3]),component(2,'StaticMesh',{'asset':model,'primitive':0,'visible':False}),component(2,'AcousticMaterial',material()),transform(101,[0,1.5,-6],scale=[2,3,4])]
        r=self.requests([room,self.inspect(),transaction(1,[{'op':'component.remove','id':uid(2),'type':'AcousticMaterial'}]),self.inspect(2)])
        for item in r:self.result(item)
        self.assertEqual(self.result(r[1])['triangles'],120);self.assertEqual(self.result(r[1])['sources'][0]['direct_visibility'],0);self.assertEqual(self.result(r[3])['sources'][0]['direct_visibility'],1)
        self.assertEqual(self.result(r[1])['geometry'][0]['shape'],'static_mesh')

    @unittest.skipUnless(CAPS['audio_capture'],'Optional Steam Audio not built')
    def test_mix_linearity_disabled_geometry_and_capture_protection(self):
        self.import_clip();r=self.requests([self.room(),transaction(1,[component(2,'AcousticMaterial',material(enabled=False))]),self.capture('single',2),transaction(2,[create(102,'Second emitter'),transform(102,[0,1.5,-6]),component(102,'AudioEmitter',emitter(self.asset))]),self.capture('double',3),transaction(3,[component(102,'AudioEmitter',emitter(self.asset,enabled=False))]),self.capture('one-again',4),self.capture('single',4)])
        for item in r[:-1]:self.result(item)
        self.error(r[-1],-32602);self.assertEqual(self.result(r[2])['triangles'],0)
        single=pcm(self.directory/'single.wav');double=pcm(self.directory/'double.wav');self.assertEqual(single,pcm(self.directory/'one-again.wav'));self.assertLess(max(abs(b-2*a) for a,b in zip(single,double)),1e-7)
        bad=self.capture('bad',4);bad['params']['path']=native(self.world)
        asset_path=next(self.directory.rglob(self.asset+'.paudio'));reserved=self.capture('bad',4);reserved['params']['path']=native(asset_path.parent/'new.wav')
        r=self.requests([bad,reserved]);self.error(r[0],-32602);self.error(r[1],-32602)

if __name__=='__main__':
    import sys
    result=unittest.main(argv=['audio_contract'],verbosity=2,exit=False).result
    if args.evidence:
        images={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in SCRATCH.rglob('*.wav')}
        args.evidence.parent.mkdir(parents=True,exist_ok=True);args.evidence.write_text(json.dumps({'binary':BINARY,'binary_sha256':hashlib.sha256(Path(BINARY).read_bytes()).hexdigest(),'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'passed':result.wasSuccessful(),'tests_run':result.testsRun,'skips':result.skipped,'run':str(SCRATCH),'calls':EVIDENCE,'wave_files':images},indent=2)+'\n')
    sys.exit(0 if result.wasSuccessful() else 1)
