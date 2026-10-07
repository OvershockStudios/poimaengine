#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual JSON-RPC scene-product readbacks; four small captures, no performance claim."""
import argparse,hashlib,json,math,subprocess,sys,traceback,uuid
from pathlib import Path
from texture_fixture import quad
from gltf_fixture import glb
from scene_capture import pixels

def check(ok,why):
    if not ok:raise RuntimeError(str(why))
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('binary',type=Path);p.add_argument('--output',type=Path,required=True);p.add_argument('--gpu',type=int,default=0);p.add_argument('--windows-interop',action='store_true');a=p.parse_args()
    check(not sys.flags.optimize,'Shared BMP parser requires assertions')
    run=a.output.resolve()/uuid.uuid4().hex;run.mkdir(parents=True);world=run/'world.json';camera=uuid.uuid4().hex
    sources={n:Path(__file__).with_name(n) for n in ('scene_product_probes.py','scene_capture.py','texture_fixture.py','gltf_fixture.py')}
    report={'passed':False,'runs':[],'captures':{},'checks':[],'gpu_index':a.gpu,'binary_sha256':hashlib.sha256(a.binary.read_bytes()).hexdigest(),'source_sha256':{n:hashlib.sha256(p.read_bytes()).hexdigest()for n,p in sources.items()},'limitations':['Four small Vulkan RPC captures on the selected actual device; no performance or physical-input claim.','Capture RPC renders two frames in a new context: valid stationary second-frame correspondence is expected, not cross-request motion or first-submission validity.','Sparse raw attachment values are checked independently of display encoding; full floating-point image export and disocclusion are not qualified.']}
    def native(path):return subprocess.check_output(['wslpath','-w',str(path.resolve())],text=True).strip()if a.windows_interop else str(path.resolve())
    def batch(requests):
        req=[{'jsonrpc':'2.0','id':i,'method':m,'params':v}for i,(m,v)in enumerate(requests)];entry={'requests':req};report['runs'].append(entry)
        r=subprocess.run([str(a.binary.resolve()),'world',native(world)],input=''.join(json.dumps(x)+'\n'for x in req),capture_output=True,text=True,encoding='utf-8',timeout=180);entry.update(exit_code=r.returncode,stdout=r.stdout,stderr=r.stderr);check(r.returncode==0,'Engine process failure')
        rows=[json.loads(x)for x in r.stdout.splitlines()];check(len(rows)==len(req),'Response count')
        for x,y in zip(req,rows):check(y.get('jsonrpc')=='2.0' and y.get('id')==x['id'] and 'result'in y,y)
        return [x['result']for x in rows]
    def capture(name,view,probes,method='world.capture',extra=None):
        params={'revision':1,'camera':camera,'path':native(run/(name+'.bmp')),'width':320,'height':240,'gpu':a.gpu,'samples':1,'scene_debug_view':view,'scene_product_probes':probes}
        if method=='runtime.capture':params.pop('revision')
        params.update(extra or {});return method,params
    def validate(name,result,points,view):
        check(result['capture_written']and result['hardware']and result['nvrhi_errors']==0,result);check(result['samples']==1 and result['frames_presented']==2,'RPC capture frame/sample contract changed')
        check(report.setdefault('gpu_name',result['gpu'])==result['gpu'],'Device changed')
        product=result['render_diagnostics']['scene_products'];check(product['available']and product['motion_available']and product['view']==view,product)
        check(product['normal_format']=='RGBA16_FLOAT' and product['normal_space']=='world' and product['normal_alpha']=='surface validity','Normal metadata')
        check(product['depth_convention']=='device depth [0,1], near 0, far/clear 1','Depth metadata')
        check(product['motion_convention']=='previous unjittered UV minus current unjittered UV; top-left scene render viewport','Motion convention')
        check(product['history_sequence']==2 and product['history_valid'] is True,'Fresh two-frame capture did not report its second submission')
        values=product['probes'];check(len(values)==len(points),'Probe count/order mismatch');image=pixels(run/(name+'.bmp'))
        for point,value in zip(points,values):
            check(value['x']==point['x']and value['y']==point['y'],'Probe order or duplicate lost');x,y=point['x'],point['y'];surface=x>=150 and x<=170 and y>=110 and y<=130
            check(value['surface_valid']is surface and value['motion_valid']is surface,('Validity mismatch',point,value))
            check(all(math.isfinite(v)for v in [value['depth'],*value['shading_normal'],*value['motion']]),'Nonfinite probe')
            if surface:
                # Plane z=0, camera z=4, near=.1 far=20: standard [0,1] perspective device depth.
                expected_depth=20/(20-.1)-20*.1/((20-.1)*4)
                check(abs(value['depth']-expected_depth)<2e-6,('Depth',value['depth'],expected_depth))
                check(max(abs(x-y)for x,y in zip(value['shading_normal'],[0,0,1]))<.001,'Raw world normal')
                check(max(abs(v)for v in value['motion'])<2e-6,('Static motion',value['motion']))
                expected={'motion':(128,128,0),'motion_validity':(255,255,255),'shading_normal':(128,128,255),'depth':(51,51,51)}[view]
            else:
                check(value['depth']==1 and value['shading_normal']==[0,0,0]and value['motion']==[0,0],'Invalid background not cleared');expected=(0,0,0)
            check(max(abs(v-e)for v,e in zip(image[y][x],expected))<=2,('Debug display',view,point,image[y][x],expected))
        path=run/(name+'.bmp');report['captures'][name]={'result':result,'image_sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
    try:
        doc,blob=quad([],{ 'pbrMetallicRoughness':{'baseColorFactor':[.5,.5,.5,1],'metallicFactor':0,'roughnessFactor':.6},'doubleSided':True});asset=run/'plane.glb';asset.write_bytes(glb(doc,blob))
        imported=batch([('asset.import',{'source':native(asset)})])[0]['asset']
        ops=[{'op':'entity.create','id':camera,'name':'Observer'},{'op':'component.set','id':camera,'type':'Transform','value':{'position':[0,0,4],'rotation':[0,0,0,1],'scale':[1,1,1]}},{'op':'component.set','id':camera,'type':'Camera','value':{'vertical_fov':60,'near':.1,'far':20}},{'op':'asset.instantiate','id':uuid.uuid4().hex,'name':'Analytic plane','asset':imported}]
        batch([('world.transact',{'base_revision':0,'request_id':uuid.uuid4().hex,'ops':ops})])
        base=[{'x':160,'y':120},{'x':5,'y':5},{'x':161,'y':121},{'x':160,'y':120}];maximum=(base*16)
        cases=[('normal-64','shading_normal',maximum),('motion','motion',base),('validity','motion_validity',base)]
        rows=batch([capture(name,view,points)for name,view,points in cases])
        for (name,view,points),row in zip(cases,rows):validate(name,row,points,view)
        sid=uuid.uuid4().hex;rows=batch([('runtime.start',{'session_id':sid,'revision':1}),('runtime.step',{'session_id':sid,'request_id':uuid.uuid4().hex,'expected_tick':0,'ticks':3}),capture('runtime-depth','depth',base,'runtime.capture',{'session_id':sid,'tick':3})]);validate('runtime-depth',rows[-1],base,'depth')
        report['checks']=['Raw analytic perspective depth and world normal agree at covered interior pixels.','Sky/background depth, normal and motion clear with both validity flags false.','64 probes accepted; duplicate coordinates and input ordering preserved.','Motion and validity display agree with raw stationary valid motion; source captures restart their two-submission sequence.','Actual runtime.capture at tick3 returns the same analytic products.'];report['passed']=True
    except BaseException:report['error']=traceback.format_exc()
    finally:
        report['inputs_unchanged']=report['binary_sha256']==hashlib.sha256(a.binary.read_bytes()).hexdigest()and all(hashlib.sha256(p.read_bytes()).hexdigest()==report['source_sha256'][n]for n,p in sources.items())
        if not report['inputs_unchanged']:report['passed']=False
        (run/'evidence.json').write_text(json.dumps(report,indent=2)+'\n')
    check(report['passed'],str(run/'evidence.json'));print(json.dumps({'passed':True,'evidence':str(run/'evidence.json')}))
if __name__=='__main__':main()
