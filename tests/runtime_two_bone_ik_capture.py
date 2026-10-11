#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Original IK ribbon versus independently authored static positions; external Job required."""
import argparse,copy,json,math,os,struct,sys,time,traceback,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'));sys.path.insert(0,str(ROOT/'tests'))
from live_material_capture import Owner,need,sha,encoded,same,compare
from shadow_controls_capture import component,transform,primitive
from animation_fixture import ribbon
from gltf_fixture import glb
from scene_capture import pixels
W,H,CAP=320,240,64*1048576
CAM,LIGHT,ENV,RIG=(f'{n:032x}' for n in (1,2,3,400))
POINTS=[[-.5,0,0],[.5,0,0],[-.25,1.25,0],[.25,.75,0],[1,1.5,0],[1,.5,0]]
class BoundedOwner(Owner):
    def raw(self,row):
        need(len(encoded(row))<=1048576,'Raw RPC row exceeds1MiB.')
        super().raw(row)
def near(a,b,epsilon=1e-6):need(len(a)==len(b) and all(math.isfinite(x) and abs(x-y)<=epsilon for x,y in zip(a,b)),'Independent analytic oracle differs.')
def source_parts():
 doc,raw=ribbon();doc['nodes'][0]['translation']=[1,0,0];doc['nodes'][1]['children']=[3];doc['nodes'].append(dict(name='Effector',translation=[0,1,0]))
 reference=copy.deepcopy(doc);data=bytearray(raw);attrs=reference['meshes'][0]['primitives'][0]['attributes'];position=reference['accessors'][attrs['POSITION']];view=reference['bufferViews'][position['bufferView']];offset=view.get('byteOffset',0)+position.get('byteOffset',0)
 struct.pack_into('<18f',data,offset,*[x for row in POINTS for x in row]);position.update(min=[-.5,0,0],max=[1,1.5,0])
 del attrs['JOINTS_0'];del attrs['WEIGHTS_0'];reference['nodes']=[dict(name='Independent deformed reference',mesh=0)];reference['scenes']=[dict(nodes=[0])];reference.pop('skins');reference.pop('animations')
 return (doc,raw),(reference,bytes(data))
def case(args,role,parts,deadline):
 output=args.output/role;output.mkdir();(output/'captures').mkdir();local=argparse.Namespace(**vars(args));local.output=output
 r=dict(passed=False,role=role,calls=[],transport=[],captures=[],native_owner=None);owner=None;error=None
 try:
  source=output/'s.glb';source.write_bytes(glb(*parts));r['source_sha256']=sha(source)
  owner=BoundedOwner(local,r,deadline);discovery=owner.rpc('world.describe',dict(view='method',name='runtime.ik'));need(discovery['schema_revision']==87,'IK schema differs.')
  asset=owner.rpc('asset.import',dict(source=str(source)))['asset'];ops=[]
  for entity in (CAM,LIGHT,ENV):ops.extend([dict(op='entity.create',id=entity,name='Analytic IK '+entity),transform(entity)])
  ops.extend([transform(CAM,(.25,.8,6)),component(CAM,'Camera',dict(vertical_fov=35,near=.1,far=20)),transform(LIGHT,(0,1,3)),
   component(LIGHT,'Light',dict(kind='point',color=[1,1,1],intensity=12*math.pi,enabled=True,shadow=dict(enabled=False,distance=20,bias=.0001,normal_bias=.005))),
   component(ENV,'LightingEnvironment',dict(ambient=[.03]*3,exposure=1)),dict(op='asset.instantiate',id=RIG,asset=asset,name=role)])
  owner.edit(ops)
  if role=='ik':
   authored=owner.rpc('entity.get',dict(id=RIG,revision=owner.revision))['value']['components']['AnimationRig'];rig=copy.deepcopy(authored);rig.update(clip=None,playing=False,ik=[dict(slot=1,root=2,middle=1,end=3,target=[1,1,0],pole=[0,2,0],space='rig')]);owner.edit([component(RIG,'AnimationRig',rig)])
  r['world_sha256']=sha(output/'world.json');owner.session=uuid.uuid4().hex;start=owner.rpc('runtime.start',dict(session_id=owner.session,revision=owner.revision));need(start['started'] is True and start['tick']==0,'Runtime start differs.')
  if role=='ik':
   state=owner.rpc('runtime.ik',dict(session_id=owner.session,tick=0,entity=RIG,slot=1))['state'];near(state['end_position'],[1,1,0]);need(state['clamped'] is False and state['pole_fallback'] is False and state['space']=='rig' and state['target_error']<=1e-6,'Independent solve diagnostics differ.');r['ik_state']=state
   nodes=owner.rpc('entity.query',dict(revision=owner.revision,component='RigNode',limit=64));need(nodes['next_after'] is None and len(nodes['entities'])==4,'Rig mapping differs.')
   matrices={}
   for item in nodes['entities']:
    binding=owner.rpc('entity.get',dict(id=item['id'],revision=owner.revision))['value']['components']['RigNode'];need(binding['rig']==RIG,'Foreign rig node.');matrices[str(binding['node'])]=owner.rpc('runtime.entity',dict(session_id=owner.session,tick=0,id=item['id']))['world_matrix']
   identity=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1];middle=[0,-1,0,0,1,0,0,0,0,0,1,0,0,1,0,1]
   near(matrices['2'],identity);near(matrices['1'],middle);near(matrices['3'][12:15],[1,1,0]);near(matrices['0'][12:15],[1,0,0]);r['rig_world_matrices']=matrices
  path=output/'captures/c.bmp';params=dict(camera=CAM,path=str(path),gpu=args.gpu,width=W,height=H,samples=1,lighting_path=args.lighting_path,frames_in_flight=2,capture_frames=2,ambient_occlusion=dict(mode='none'),reconstruction='none',shadow_filter='pcf3_point',session_id=owner.session,tick=0,material_revision=0,scene_product_probes=[dict(x=x,y=y) for y in (H//2-12,H//2,H//2+12) for x in (W//2-20,W//2,W//2+20)])
  result=owner.rpc('runtime.capture',params);need(result['capture_written'] is True and result['hardware'] is True and result['nvrhi_errors']==0 and result['build_version']=='0.0.118' and result['session_id']==owner.session and result['tick']==0 and result['material_revision']==0 and result['object_count']==1 and result['width']==W and result['height']==H and result['samples']==1 and isinstance(result['gpu'],str) and bool(result['gpu']),'Actual hardware capture differs.')
  d=result['render_diagnostics'];need(d['lighting_path']==args.lighting_path and d['ambient_occlusion']['mode']=='none' and d['reconstruction']['mode']=='none','Render recipe differs.');counts=d['last_draws'];need(counts['objects']==counts['camera_draws']==1 and counts['camera_triangles']==4 and counts['skinned_instances']==(1 if role=='ik' else 0) and (role!='ik' or counts['skinned_vertices']>=6),'Actual skin/reference draw path differs.')
  receipts=d['scene_receipts'];need(same(receipts['submitted'],receipts['presented']),'Frame receipt mismatch.');receipt=receipts['presented'];need(receipt['material_revision']==0 and receipt['camera_id']==CAM and receipt['authored_revision']==owner.revision and receipt['frame_sequence']>0 and receipt['submission_id']>0 and bool(receipt['presentation_source_id']),'Exact scene receipt missing.')
  observed=copy.deepcopy(receipts['observed']);presented=copy.deepcopy(receipt)
  for row in (observed,presented):row.pop('frame_sequence');row.pop('submission_id')
  need(same(observed,presented),'Observed scene differs from presented.');products=d['scene_products'];need(products['available'] is True and len(products['probes'])==9,'Native products missing.');r['probes']=copy.deepcopy(products['probes'])
  image=pixels(path);background=image[0][0];need(sum(pixel!=background for row in image for pixel in row)>=500,'Empty background cannot qualify.');need(sha(output/'world.json')==r['world_sha256'],'Capture mutated world.');r['captures'].append(dict(label=role,image='captures/c.bmp',sha256=sha(path),bytes=path.stat().st_size,result=result));r['passed']=True
 except BaseException as failure:error=repr(failure);r['error']=error;r['traceback']=traceback.format_exc()
 finally:
  if owner is not None:
   try:owner.close()
   except BaseException as failure:r['cleanup_error']=repr(failure);error=error or repr(failure)
  r['passed']=bool(r['passed'] and error is None and r['native_owner'] is not None)
 return r

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--gpu',type=int,required=True);p.add_argument('--lighting-path',choices=('forward','deferred'),required=True);a=p.parse_args();a.binary=a.binary.resolve();a.output=a.output.resolve();need(os.name=='nt' and not sys.flags.optimize and a.gpu>=0 and not a.output.exists(),'Native owned fresh fixture required.');a.output.mkdir(parents=True);start=time.monotonic();deadline=start+300
 files=[Path(__file__),ROOT/'tests/live_material_capture.py',ROOT/'tests/shadow_controls_capture.py',ROOT/'tests/animation_fixture.py',ROOT/'tests/gltf_fixture.py',ROOT/'tests/scene_capture.py',a.binary,a.binary.parent/'phonon.dll']+[x for x in (ROOT/'tools/python/poima_client').rglob('*') if x.is_file() and x.suffix in ('.py','.json')];pins={str(x):sha(x) for x in files};r=dict(passed=False,recipe=dict(gpu=a.gpu,lighting_path=a.lighting_path,width=W,height=H,maximum_rgb_error=1,maximum_hdr_error=.002,maximum_depth_error=1e-5,maximum_normal_error=1e-5),scope='Independent explicit synthetic geometry; no player, latency, grounding or character-art claim.',source_pins=pins,cases=[])
 try:
  for role,parts in zip(('ik','reference'),source_parts()):r['cases'].append(case(a,role,parts,deadline));need(r['cases'][-1]['passed'],'Native capture case failed: '+role)
  left,right=r['cases'];need(left['world_sha256']!=right['world_sha256'] and left['source_sha256']!=right['source_sha256'] and left['captures'][0]['result']['gpu']==right['captures'][0]['result']['gpu'],'Distinct source/reference or fixed adapter missing.')
  rgb=compare(pixels(a.output/'ik/captures/c.bmp'),pixels(a.output/'reference/captures/c.bmp'));need(rgb['max_rgb_delta']<=1,'Full BMP independent geometry differs.');valid=0;errors=dict(raw_hdr=0.0,depth=0.0,shading_normal=0.0)
  for x,y in zip(left['probes'],right['probes']):
   need((x['x'],x['y'],x['surface_valid'])==(y['x'],y['y'],y['surface_valid']),'Probe coverage differs.')
   if not x['surface_valid']:continue
   valid+=1
   for key in errors:
    xs=x[key] if isinstance(x[key],list) else [x[key]];ys=y[key] if isinstance(y[key],list) else [y[key]];near(xs,ys,.002 if key=='raw_hdr' else 1e-5);errors[key]=max(errors[key],max(abs(u-v) for u,v in zip(xs,ys)))
  need(valid>=3,'Insufficient independent visible HDR/normal/depth probes.');r['comparison']=dict(rgb=rgb,probe_max_errors=errors,valid_probes=valid);r['passed']=True
 except BaseException:r['error']=traceback.format_exc()
 finally:
  r['seconds']=time.monotonic()-start;r['changed_sources']=[name for name,value in pins.items() if not Path(name).is_file() or sha(name)!=value];r['passed']=bool(r['passed'] and not r['changed_sources'] and r['seconds']<=300);data=encoded(r)+b'\n';need(len(data)<=16*1048576 and sum(x.stat().st_size for x in a.output.rglob('*') if x.is_file())+len(data)<=CAP,'Final evidence/output cap exceeded.');pending=a.output/'evidence.pending.json';pending.write_bytes(data)
  if time.monotonic()>deadline:r['passed']=False;r['publication_error']='Whole300s exceeded during publication.';pending.write_bytes(encoded(r)+b'\n')
  pending.rename(a.output/'evidence.json')
  if time.monotonic()>deadline and r['passed']:r['passed']=False;r['publication_error']='Whole300s exceeded after publication.';(a.output/'evidence.json').write_bytes(encoded(r)+b'\n')
 print(json.dumps(dict(passed=r['passed'],gpu=a.gpu,lighting_path=a.lighting_path)));return 0 if r['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
