#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Independent projection/self-depth and world-product isolation; externally owned Windows Job required."""
import argparse,copy,json,os,sys,time,traceback,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'));sys.path.insert(0,str(ROOT/'tests'))
from live_material_capture import Owner,need,sha,encoded,same,compare
from shadow_controls_capture import component,transform
from texture_fixture import quad
from gltf_fixture import glb
from scene_capture import pixels
W,H,CAP=320,240,96*1048576
CAM,WALL,RED,BLUE,BODY,ENV=(f'{n:032x}' for n in (1,20,21,22,23,30))
MATERIAL=dict(pbrMetallicRoughness=dict(baseColorFactor=[0,0,0,1],metallicFactor=0,roughnessFactor=1),doubleSided=True)
def material(color):return dict(base_color=[0,0,0],emissive=color,metallic=0,roughness=1,double_sided=True)
def case(args,label,world_fov,foreground_fov,reference,deadline,world_only=False):
 output=args.output/label;output.mkdir();(output/'captures').mkdir();local=argparse.Namespace(**vars(args));local.output=output
 record=dict(passed=False,label=label,calls=[],transport=[],captures=[],native_owner=None,recipe=dict(world_fov=world_fov,foreground_fov=foreground_fov,reference=reference,world_only=world_only));owner=None;failure=None
 try:
  source=output/'analytic.glb';source.write_bytes(glb(*quad([],MATERIAL)));record['source_sha256']=sha(source)
  owner=Owner(local,record,deadline);discovery=owner.rpc('world.describe',dict(view='component',name='CameraDraw'));need(discovery['schema_revision']==89,'CameraDraw discovery revision differs')
  asset=owner.rpc('asset.import',dict(source=str(source)))['asset'];ops=[]
  entities=[(CAM,None),(WALL,None),(ENV,None)]
  if not world_only:entities.extend([(RED,None if reference else CAM),(BLUE,None if reference else CAM)])
  if not reference:entities.append((BODY,None))
  for identity,parent in entities:
   ops.append(dict(op='entity.create',id=identity,name='Analytic '+identity,parent=parent));ops.append(transform(identity))
  lens=dict(vertical_fov=world_fov,near=.01,far=20)
  if not reference and not world_only:lens['viewmodel']=dict(vertical_fov=foreground_fov,near=.01,far=10)
  ops.extend([component(CAM,'Camera',lens),component(ENV,'LightingEnvironment',dict(ambient=[0,0,0],exposure=1))])
  geometry=[(WALL,(0,0,-5 if reference else -.2),(20,20,1),[.03125]*3)]
  if not world_only:geometry.extend([(RED,(.1,-.06,-.4),(.12,.1,1),[.5,0,0]),(BLUE,(-.05,.03,-.6),(.22,.16,1),[0,0,.5])])
  if not reference:geometry.append((BODY,(0,0,-.12),(.15,.1,1),[0,.5,0]))
  for identity,position,scale,color in geometry:
   ops.extend([transform(identity,position,scale),component(identity,'StaticMesh',dict(asset=asset,primitive=0,visible=True,cast_shadows=False)),component(identity,'PbrMaterial',material(color))])
   if identity in (RED,BLUE) and not reference:ops.append(component(identity,'CameraDraw',dict(camera=CAM,mode='viewmodel')))
   if identity==BODY:ops.append(component(identity,'CameraDraw',dict(camera=CAM,mode='hidden')))
  owner.edit(ops);record['world_sha256']=sha(output/'world.json');owner.session=uuid.uuid4().hex
  start=owner.rpc('runtime.start',dict(session_id=owner.session,revision=owner.revision));need(start['started'] is True and start['tick']==0,'Actual owner did not start')
  path=output/'captures/c.bmp';params=dict(session_id=owner.session,tick=0,material_revision=0,camera=CAM,path=str(path),gpu=args.gpu,width=W,height=H,samples=1,lighting_path=args.lighting_path,frames_in_flight=2,capture_frames=2,ambient_occlusion=dict(mode='none'),reconstruction='none',scene_product_probes=[dict(x=x,y=y) for y in (H//2-16,H//2,H//2+16) for x in (W//2-24,W//2,W//2+24)])
  result=owner.rpc('runtime.capture',params);need(result['build_version']=='0.0.120' and result['hardware'] is True and result['capture_written'] is True and result['nvrhi_errors']==0,'Hardware capture missing')
  need(result['width']==W and result['height']==H and result['samples']==1 and result['session_id']==owner.session and result['tick']==0 and result['material_revision']==0,'Capture owner/extent differs')
  diagnostics=result['render_diagnostics'];need(diagnostics['lighting_path']==args.lighting_path,'Rendering path differs')
  receipts=diagnostics['scene_receipts'];need(same(receipts['submitted'],receipts['presented']),'Submitted/presented frame differs')
  observed=copy.deepcopy(receipts['observed']);presented=copy.deepcopy(receipts['presented'])
  for receipt in (observed,presented):receipt.pop('frame_sequence');receipt.pop('submission_id')
  need(same(observed,presented),'Actual observed/presented immutable snapshot differs')
  receipt=receipts['presented'];need(receipt['camera_id']==CAM and receipt['authored_revision']==owner.revision and receipt['material_revision']==0 and receipt['frame_sequence']>0 and receipt['submission_id']>0,'Exact frame identity missing')
  if reference:need('camera_draw' not in receipt,'Independent reference unexpectedly used foreground path')
  else:
   draw=receipt['camera_draw'];need(draw['hidden']==[dict(entity_id=BODY,incarnation=1)],'Camera-hidden body receipt differs')
   expected=[] if world_only else [dict(entity_id=identity,incarnation=1) for identity in (RED,BLUE)]
   need(draw['viewmodels']==expected,'Actual foreground membership differs')
   need(draw['viewmodel_lens']==(None if world_only else dict(vertical_fov=foreground_fov,near=.01,far=10)),'Actual foreground lens differs')
  record['probes']=copy.deepcopy(diagnostics['scene_products']['probes']);need(len(record['probes'])==9 and all(p['surface_valid'] for p in record['probes']),'World wall products absent')
  record['captures'].append(dict(image='captures/c.bmp',bytes=path.stat().st_size,sha256=sha(path),result=result))
  need(sha(output/'world.json')==record['world_sha256'],'Capture changed authored source');record['passed']=True
 except BaseException as error:failure=repr(error);record['error']=failure;record['traceback']=traceback.format_exc()
 finally:
  if owner is not None:
   try:owner.close()
   except BaseException as error:failure=failure or repr(error);record['cleanup_error']=repr(error)
  record['passed']=bool(record['passed'] and failure is None and record['native_owner'] is not None)
 return record
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--binary',required=True,type=Path);parser.add_argument('--output',required=True,type=Path);parser.add_argument('--gpu',required=True,type=int);parser.add_argument('--lighting-path',required=True,choices=('forward','deferred'));args=parser.parse_args()
 args.binary=args.binary.resolve();args.output=args.output.resolve();need(os.name=='nt' and not sys.flags.optimize and not args.output.exists(),'Fresh root-owned Windows fixture required');args.output.mkdir(parents=True)
 sources=[Path(__file__),args.binary,args.binary.parent/'phonon.dll',*[ROOT/'tests'/name for name in ('live_material_capture.py','shadow_controls_capture.py','texture_fixture.py','gltf_fixture.py','scene_capture.py')]]
 sources.extend(p for p in (ROOT/'tools/python/poima_client').rglob('*') if p.is_file() and p.suffix in ('.py','.json'));pins={str(p):sha(p) for p in sources};started=time.monotonic();deadline=started+300
 record=dict(passed=False,source_pins=pins,recipe=dict(gpu=args.gpu,lighting_path=args.lighting_path,width=W,height=H,maximum_rgb_error=1),cases=[],comparisons=[],scope='Original analytic opaque quads; independent lens projection and self-depth, a closer wall, hidden-body camera exclusion and world-product isolation. No character art, AA, shadows, physical weapon collision, window continuity or performance claim.')
 try:
  recipes=[('foreground75',60,75,False,False),('reference75',75,None,True,False),('world-only',60,None,False,True),('world110-foreground75',110,75,False,False),('foreground50',60,50,False,False),('reference50',50,None,True,False)]
  for label,world_fov,foreground_fov,reference,world_only in recipes:
   record['cases'].append(case(args,label,world_fov,foreground_fov,reference,deadline,world_only));need(record['cases'][-1]['passed'],'Actual capture case failed: '+label)
  images={c['label']:pixels(args.output/c['label']/'captures/c.bmp') for c in record['cases']}
  for left,right in [('foreground75','reference75'),('foreground50','reference50'),('foreground75','world110-foreground75')]:
   comparison=compare(images[left],images[right]);need(comparison['max_rgb_delta']<=1,'Independent projection/world FOV separation differs');record['comparisons'].append(dict(left=left,right=right,**comparison))
  changed=compare(images['foreground75'],images['foreground50']);need(changed['strong_changed_pixels']>=100,'Foreground FOV did not change visible coverage');record['lens_change']=changed
  image=images['foreground75'];red=sum(r>80 and g<5 and b<5 for row in image for r,g,b in row);blue=sum(b>80 and r<5 and g<5 for row in image for r,g,b in row)
  need(red>=100 and blue>=100,'Foreground empty/one layer: self-depth oracle cannot qualify');record['color_coverage']=dict(red_pixels=red,blue_pixels=blue)
  fg,world=record['cases'][0],record['cases'][2]
  fields=('x','y','depth','raw_hdr','resolved_hdr','shading_normal','surface_valid','motion','motion_valid')
  need(all(same({k:a[k] for k in fields},{k:b[k] for k in fields}) for a,b in zip(fg['probes'],world['probes'])),'Foreground contaminated world products');record['world_product_isolation']=dict(exact_probes=9,fields=list(fields));record['passed']=True
 except BaseException:record['error']=traceback.format_exc()
 finally:
  record['seconds']=time.monotonic()-started;record['changed_sources']=[name for name,digest in pins.items() if not Path(name).is_file() or sha(name)!=digest]
  record['passed']=bool(record['passed'] and not record['changed_sources'] and record['seconds']<=300);data=encoded(record)+b'\n';need(sum(p.stat().st_size for p in args.output.rglob('*') if p.is_file())+len(data)<=CAP,'Output cap exceeded');(args.output/'evidence.json').write_bytes(data)
  if time.monotonic()>deadline:
   record['passed']=False;record['publication_error']='Whole300s exceeded during publication.';(args.output/'evidence.json').write_bytes(encoded(record)+b'\n')
 print(json.dumps(dict(passed=record['passed'],gpu=args.gpu,lighting_path=args.lighting_path)));return 0 if record['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
