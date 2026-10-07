#!/usr/bin/env python3
"""Vulkan compute skinning: independent reference, tangent frames and shadow-only work."""
# SPDX-License-Identifier: Apache-2.0
import argparse,base64,copy,hashlib,json,math,struct,subprocess,uuid
from pathlib import Path
from animation_fixture import ribbon
from gltf_fixture import glb
from texture_fixture import png
from scene_capture import pixels
parser=argparse.ArgumentParser();parser.add_argument('binary',type=Path);parser.add_argument('--output',type=Path,required=True);parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--gpu',type=int,default=0);parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
args=parser.parse_args();run=args.output/uuid.uuid4().hex;run.mkdir(parents=True);world=run/'skinning.world.json';revision=0
record={'passed':False,'lighting_path':args.lighting_path,'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),'runs':[],'comparisons':{},'captures':[]}
def uid(n):return f'{n:032x}'
def native(p):return subprocess.check_output(['wslpath','-w',str(p.resolve())],text=True).strip() if args.windows_interop else str(p.resolve())
def batch(requests,error=None):
    rows=[{'jsonrpc':'2.0','id':i,'method':m,'params':p} for i,(m,p) in enumerate(requests)]
    p=subprocess.run([str(args.binary.resolve()),'world',native(world)],input=''.join(json.dumps(r)+'\n' for r in rows),capture_output=True,text=True,timeout=120)
    replies=[json.loads(s) for s in p.stdout.splitlines()];record['runs'].append({'requests':rows,'responses':replies,'exit_code':p.returncode,'stderr':p.stderr})
    assert p.returncode==0 and len(replies)==len(rows),p.stdout+p.stderr
    if error is not None:
        assert len(replies)==1 and replies[0]['error']['code']==error,replies
        return replies
    for r in replies:assert 'result' in r,r
    return [r['result'] for r in replies]
def component(n,kind,value):return {'op':'component.set','id':uid(n),'type':kind,'value':value}
def transform(n,p=(0,0,0),q=(0,0,0,1),s=(1,1,1)):return component(n,'Transform',{'position':p,'rotation':q,'scale':s})
def edit(ops):
    global revision
    batch([('world.transact',{'request_id':uuid.uuid4().hex,'base_revision':revision,'ops':ops})]);revision+=1

def asset(name,doc,blob):
    path=run/(name+'.glb');path.write_bytes(glb(doc,blob));result=batch([('asset.import',{'source':native(path)})])[0]['asset'];path.unlink();return result

def capture(name,asset,mode='gpu',culling=True,error=None):
    path=run/(name+'.bmp');params={'revision':revision,'camera':uid(1),'path':native(path),'width':640,'height':480,'gpu':args.gpu,'samples':1 if args.lighting_path=='deferred' else 4,'lighting_path':args.lighting_path,'profile':True,'asset':asset,'clip':0,'time':1,'skinning':mode,'culling':culling}
    before=world.read_bytes();result=batch([('asset.animation.capture',params)],error=error)[0];assert before==world.read_bytes()
    if error is not None:assert not path.exists();return result
    assert result['capture_written'] and result['hardware'] and result['nvrhi_errors']==0,result
    assert result['render_diagnostics']['lighting_path']==args.lighting_path,result
    assert result['samples']==(1 if args.lighting_path=='deferred' else 4),result
    assert result['animation']['skinning']==('gpu_compute' if mode=='gpu' else 'cpu_reference')
    assert result['render_diagnostics']['last_draws']['skinned_instances']==0 if mode=='cpu' else result['render_diagnostics']['last_draws']['skinned_instances']>0
    record['captures'].append(result);return result

def compare(name,a,b,limit=128):
    left,right=pixels(run/(a+'.bmp')),pixels(run/(b+'.bmp'))
    errors=[max(abs(x-y) for x,y in zip(p,q)) for row,other in zip(left,right) for p,q in zip(row,other)]
    result={'maximum_channel_error':max(errors),'pixels_over_2':sum(v>2 for v in errors),'changed_pixels':sum(v!=0 for v in errors),'mean_max_channel_error':sum(errors)/len(errors)}
    record['comparisons'][name]=result
    if limit is not None:assert result['pixels_over_2']<=limit and result['mean_max_channel_error']<=.1,(name,result)
    return result

def append_accessor(doc,blob,rows,kind,fmt='f'):
    while len(blob)%4:blob.append(0)
    offset=len(blob);flat=[x for row in rows for x in row];blob.extend(struct.pack('<'+fmt*len(flat),*flat))
    doc['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':len(blob)-offset})
    doc['accessors'].append({'bufferView':len(doc['bufferViews'])-1,'componentType':5126 if fmt=='f' else 5123,'count':len(rows),'type':kind})
    doc['buffers'][0]['byteLength']=len(blob)
    if kind=='VEC3':doc['accessors'][-1].update(min=[min(r[k] for r in rows) for k in range(3)],max=[max(r[k] for r in rows) for k in range(3)])
    return len(doc['accessors'])-1

try:
    edit([{'op':'entity.create','id':uid(1),'name':'Camera'},transform(1,(.5,1,5)),component(1,'Camera',{'vertical_fov':50,'near':.1,'far':100})])
    doc,blob=ribbon();base=asset('ribbon',doc,blob)
    first=capture('base-gpu',base);capture('base-cpu',base,'cpu');capture('base-unculled',base,culling=False)
    compare('base_cpu_gpu','base-gpu','base-cpu',0);compare('base_culling','base-gpu','base-unculled',0)
    assert first['render_diagnostics']['last_draws']['skinned_vertices']==6
    assert first['render_diagnostics']['gpu']['skinning']['samples']>=1
    # Nonuniform joint scale, rotation and a tangent-space normal map exercise
    # inverse-transpose normals and re-orthogonalized tangent directions.
    bent=copy.deepcopy(doc);bent['nodes'][2]['scale']=[.9,1.1,.8];bent['nodes'][1]['scale']=[.8,1.3,1.1]
    bent['nodes'][1]['rotation']=[0,math.sin(.3),0,math.cos(.3)]
    bent['images']=[{'uri':'data:image/png;base64,'+base64.b64encode(png(1,1,[204,76,230,255])).decode()}];bent['textures']=[{'source':0}];bent['materials'][0]['normalTexture']={'index':0}
    normal=asset('normal-frame',bent,blob);capture('normal-gpu',normal);capture('normal-cpu',normal,'cpu');compare('normal_frames','normal-gpu','normal-cpu')
    # Same source vertices, distinct palettes and output buffers.
    instanced=copy.deepcopy(doc);instanced['nodes'][2]['translation']=[-1,0,0]
    instanced['nodes'] += [{'name':'Second mesh','mesh':0,'skin':1,'translation':[-7,0,0]}, {'name':'Second tip','translation':[0,1,0],'rotation':[0,math.sin(.2),0,math.cos(.2)]}, {'name':'Second root','translation':[1.5,0,0],'children':[4]}]
    other=copy.deepcopy(instanced['skins'][0]);other.update(name='Second rig',skeleton=5,joints=[5,4]);instanced['skins'].append(other);instanced['scenes'][0]['nodes'] += [3,5]
    many=asset('two-rigs',instanced,blob);r=capture('instances-gpu',many);capture('instances-cpu',many,'cpu');compare('independent_instances','instances-gpu','instances-cpu')
    assert r['render_diagnostics']['last_draws']['skinned_instances']==2 and r['render_diagnostics']['last_draws']['skinned_vertices']==12
    # CPU-valid poses can contain a singular per-vertex blended matrix. The
    # GPU must report that failure, not publish NaNs or a substituted pose.
    singular=copy.deepcopy(doc);singular['nodes'][1]['rotation']=[0,0,1,0]
    bad=asset('singular',singular,blob);failure=capture('singular-gpu',bad,error=-32020);assert 'asset/0/0 vertex ' in failure['error']['message'],failure;capture('singular-cpu',bad,'cpu',error=-32602)
    capture('recovered',base);compare('after_failure','base-gpu','recovered',0)
    # An off-camera skinned caster still needs compute work for its shadow.
    shadow=copy.deepcopy(doc);shadow['nodes'][2]['translation']=[6,0,0];data=bytearray(blob)
    p=append_accessor(shadow,data,[(-5,-3,-1),(6,-3,-1),(-5,5,-1),(6,5,-1)],'VEC3')
    n=append_accessor(shadow,data,[(0,0,1)]*4,'VEC3');indices=append_accessor(shadow,data,[(i,) for i in [0,1,2,1,3,2]],'SCALAR','H')
    shadow['materials'].append({'pbrMetallicRoughness':{'baseColorFactor':[.7,.7,.7,1],'metallicFactor':0,'roughnessFactor':.8},'doubleSided':True})
    shadow['meshes'].append({'primitives':[{'attributes':{'POSITION':p,'NORMAL':n},'indices':indices,'material':1}]});shadow['nodes'].append({'name':'Receiver','mesh':1});shadow['scenes'][0]['nodes'].append(3)
    scene=asset('shadow-only',shadow,bytes(data));angle=math.atan(7)
    def light(enabled):return component(2,'Light',{'kind':'directional','color':[1,1,1],'intensity':math.pi,'enabled':True,'shadow':{'enabled':enabled,'distance':80,'bias':.0001,'normal_bias':.005}})
    edit([{'op':'entity.create','id':uid(2),'name':'Sun'},transform(2,q=(0,math.sin(angle/2),0,math.cos(angle/2))),light(True),{'op':'entity.create','id':uid(3),'name':'Environment'},component(3,'LightingEnvironment',{'ambient':[.02,.02,.02],'exposure':1,'shadow_resolution':1024})])
    shadow_report=capture('shadow-gpu',scene);capture('shadow-cpu',scene,'cpu');capture('shadow-unculled',scene,culling=False)
    compare('shadow_cpu_gpu','shadow-gpu','shadow-cpu');compare('shadow_culling','shadow-gpu','shadow-unculled',0)
    counts=shadow_report['render_diagnostics']['last_draws'];assert counts['camera_culled']==1 and counts['camera_draws']==1 and counts['skinned_instances']==1,counts
    edit([light(False)]);unlit=capture('shadow-off',scene,mode='cpu');difference=compare('shadow_present','shadow-gpu','shadow-off',None);assert difference['pixels_over_2']>100,difference
    record.update(passed=True,source_independent=True,world_unchanged_by_capture=True)
finally:
    (run/'evidence.json').write_text(json.dumps(record,indent=2)+'\n');print(run/'evidence.json')
