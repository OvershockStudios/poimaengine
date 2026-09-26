#!/usr/bin/env python3
"""Compare reference skinning on Vulkan with independently baked geometry."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import uuid
from animation_fixture import ribbon
from gltf_fixture import glb
from scene_capture import pixels
parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--gpu',type=int,default=0)
args=parser.parse_args();run=args.output/uuid.uuid4().hex;run.mkdir(parents=True);world=run/'animation.world.json'
record={'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'runs':[],'passed':False}
def native(p):return subprocess.check_output(['wslpath','-w',str(p.resolve())],text=True).strip() if args.windows_interop else str(p.resolve())
def batch(requests):
    rows=[{'jsonrpc':'2.0','id':i,'method':m,'params':p} for i,(m,p) in enumerate(requests)]
    p=subprocess.run([str(args.binary.resolve()),'world',native(world)],input=''.join(json.dumps(r)+'\n' for r in rows),capture_output=True,text=True,timeout=120)
    replies=[json.loads(s) for s in p.stdout.splitlines()];record['runs'].append({'requests':rows,'responses':replies,'exit_code':p.returncode,'stderr':p.stderr})
    assert p.returncode==0 and len(replies)==len(rows),p.stdout+p.stderr
    for reply in replies:assert 'result' in reply,reply
    return [r['result'] for r in replies]
def uid(n):return f'{n:032x}'
def component(kind,value):return {'op':'component.set','id':uid(1),'type':kind,'value':value}
try:
    doc,blob=ribbon();animated=run/'ribbon.glb';animated.write_bytes(glb(doc,blob))
    # Independent bake: at t=1 tip translation is +1; weights are y/2.
    # Therefore x' = x+y/2, y'=y. This is not computed by Poima's sampler.
    baked=copy.deepcopy(doc);baked.pop('skins');baked.pop('animations');baked['nodes']=[{'name':'Analytic shear','mesh':0}];baked['scenes']=[{'nodes':[0]}]
    attrs=baked['meshes'][0]['primitives'][0]['attributes'];attrs.pop('JOINTS_0');attrs.pop('WEIGHTS_0')
    position=baked['accessors'][attrs['POSITION']];view=baked['bufferViews'][position['bufferView']];binary=bytearray(blob)
    for i in range(position['count']):
        offset=view['byteOffset']+i*12;x,y,z=struct.unpack_from('<3f',binary,offset);struct.pack_into('<3f',binary,offset,x+y/2,y,z)
    position['max']=[1.5,2,0];static=run/'analytic.glb';static.write_bytes(glb(baked,bytes(binary)))
    imported=batch([('asset.import',{'source':native(p)}) for p in [animated,static]]);asset,reference=[r['asset'] for r in imported]
    batch([('world.transact',{'request_id':uuid.uuid4().hex,'base_revision':0,'ops':[
        {'op':'entity.create','id':uid(1),'name':'Camera'},component('Transform',{'position':[.5,1,5],'rotation':[0,0,0,1],'scale':[1,1,1]}),component('Camera',{'vertical_fov':50,'near':.1,'far':100}),
        {'op':'asset.instantiate','id':uid(100),'name':'Independent analytic reference','asset':reference}]})])
    before=world.read_bytes();animated.unlink();static.unlink()
    captures=[]
    for name,method,options in [('rest','asset.animation.capture',{'asset':asset,'clip':0,'time':0}),('posed','asset.animation.capture',{'asset':asset,'clip':0,'time':1}),('unculled','asset.animation.capture',{'asset':asset,'clip':0,'time':1,'culling':False}),('analytic','world.capture',{})]:
        path=run/(name+'.bmp');request={'revision':1,'camera':uid(1),'path':native(path),'width':640,'height':480,'gpu':args.gpu,'samples':4,**options}
        report=batch([(method,request)])[0];assert report['capture_written'] and report['hardware'] and report['nvrhi_errors']==0,report
        assert report['object_count']==1,report;captures.append(report)
    assert before==world.read_bytes(),'Read-only preview changed the authored scene.'
    images={name:pixels(run/(name+'.bmp')) for name in ['rest','posed','unculled','analytic']}
    def difference(a,b):
        values=[max(abs(x-y) for x,y in zip(p,q)) for row,other in zip(images[a],images[b]) for p,q in zip(row,other)]
        return {'maximum_channel_error':max(values),'pixels_over_2':sum(v>2 for v in values),'changed_pixels':sum(v!=0 for v in values),'mean_max_channel_error':sum(values)/len(values)}
    analytic=difference('posed','analytic');culling=difference('posed','unculled');motion=difference('rest','posed')
    assert analytic['pixels_over_2']<=64,analytic
    assert culling['changed_pixels']==0,culling
    assert motion['pixels_over_2']>1000,motion
    record.update(passed=True,analytic_comparison=analytic,culling_comparison=culling,motion=motion,source_independent=True,world_unchanged=True,captures=captures)
finally:
    (run/'evidence.json').write_text(json.dumps(record,indent=2)+'\n');print(run/'evidence.json')
