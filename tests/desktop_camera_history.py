#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Headless actual desktop bridge camera-cut ownership (no renderer/history claim)."""
import argparse,ctypes,hashlib,json,os,subprocess,traceback,uuid
from pathlib import Path

def check(ok,why):
    if not ok:raise RuntimeError(str(why))
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--bridge',type=Path,required=True);p.add_argument('--binary',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    check(os.name=='nt','Requires native Windows Python; no GPU rendering is requested')
    run=a.output.resolve()/uuid.uuid4().hex;run.mkdir(parents=True);host=None;lib=None;search=None;window=None;parent=None;user32=None
    paths={'bridge':a.bridge.resolve(),'binary':a.binary.resolve(),'test':Path(__file__).resolve()}
    report={'passed':False,'calls':[],'checks':[],'input_sha256':{k:hashlib.sha256(v.read_bytes()).hexdigest()for k,v in paths.items()},'limitations':['Real C ABI and a hidden owned HWND without draw; no GPU rendering, native history pixel output, physical input or GUI qualification.']}
    try:
        project=run/'project';r=subprocess.run([str(paths['binary']),'project','create',str(project),'--name','Camera cut ownership'],capture_output=True,text=True,timeout=30);check(r.returncode==0,r.stdout+r.stderr)
        manifest=json.loads((project/'project.json').read_text());world=project/manifest['entry']['world']
        search=os.add_dll_directory(str(paths['bridge'].parent));lib=ctypes.CDLL(str(paths['bridge']))
        lib.poima_desktop_create.argtypes=[ctypes.c_char_p,ctypes.c_char_p,ctypes.c_int32,ctypes.c_uint32];lib.poima_desktop_create.restype=ctypes.c_void_p
        lib.poima_desktop_call.argtypes=[ctypes.c_void_p,ctypes.c_char_p];lib.poima_desktop_call.restype=ctypes.c_void_p
        lib.poima_desktop_destroy.argtypes=[ctypes.c_void_p];lib.poima_desktop_destroy.restype=None
        host=lib.poima_desktop_create(str(world).encode(),('camera-history-'+uuid.uuid4().hex).encode(),-1,1);check(host,'Bridge creation failed')
        def rpc(method,params=None,error=False):
            req={'jsonrpc':'2.0','id':len(report['calls'])+1,'method':method,'params':params or {}}
            pointer=lib.poima_desktop_call(host,json.dumps(req).encode());check(pointer,'Bridge returned null');reply=json.loads(ctypes.string_at(pointer));report['calls'].append({'request':req,'reply':reply});check(reply.get('id')==req['id'],'Response identity')
            check(('error'in reply) if error else ('result'in reply),reply);return reply.get('error')if error else reply['result']
        def generations():
            views=rpc('desktop.inspect')['views'];return tuple(views[k]['view_cut_generation']for k in ('scene','game'))
        initial=generations();check(initial==(0,0),initial)
        schema=rpc('desktop.describe');check(schema['methods']['desktop.camera']['properties']['cut']['type']=='boolean','Missing discovery')
        rpc('desktop.camera',{'position':[3,2,7],'yaw':15});check(generations()==initial,'Ordinary navigation cut history')
        rpc('desktop.camera',{'cut':False});check(generations()==initial,'False cut changed generation')
        for params in ({'cut':1},{'cut':True,'pitch':100},{'cut':True,'unknown':1}):
            before=rpc('desktop.inspect');rpc('desktop.camera',params,error=True);after=rpc('desktop.inspect');check(before['camera']==after['camera'] and before['views']==after['views'],'Rejected cut mutated camera/history')
        rpc('desktop.camera',{'cut':True});check(generations()==(1,0),'Explicit unchanged-pose cut missing')
        rpc('desktop.camera',{'cut':True,'yaw':20});check(generations()==(2,0),'Combined pose/cut failed')
        report['checks'].append('Navigation preserves cuts; explicit cuts advance Scene only; invalid requests preserve state.')
        rev=rpc('world.inspect')['revision'];ids=[uuid.uuid4().hex for _ in range(2)];ops=[]
        for identity in ids:
            ops += [{'op':'entity.create','id':identity,'name':'Observer'},{'op':'component.set','id':identity,'type':'Camera','value':{'vertical_fov':60,'near':.1,'far':100}}]
        rev=rpc('world.transact',{'base_revision':rev,'request_id':uuid.uuid4().hex,'ops':ops})['revision']
        for cam,expected in ((ids[0],1),(ids[0],1),(ids[1],2),(ids[0],3),(None,4)):
            rpc('desktop.game.camera',{'camera':cam});check(generations()==(2,expected),'Game selection must preserve Scene and count away/back')
        before=generations();rpc('desktop.game.camera',{'camera':uuid.uuid4().hex},error=True);check(generations()==before,'Bad Game camera cut history')
        report['checks'].append('Game camera changes, deselection and away/back are isolated; same selection and failed selection do not advance.')
        # Legacy uses the actual shared viewport slot; the independent Game counter stays unchanged.
        for params,expected in (({'mode':'game','camera':ids[0]},3),({'mode':'scene'},4),({'mode':'game','camera':ids[0]},5),({'mode':'scene'},6)):
            rpc('desktop.view',params);check(generations()==(expected,4),'Legacy selection history mismatch')
        rpc('desktop.view',{'mode':'scene'});check(generations()==(6,4),'Identical legacy selection advanced')
        rpc('desktop.frame',{'revision':rev,'id':ids[0],'aspect':1});check(generations()==(7,4),'Frame did not cut Scene')
        rpc('desktop.frame',{'revision':rev,'id':ids[0],'aspect':1});check(generations()==(8,4),'Repeated frame did not cut Scene')
        before=generations();rpc('desktop.frame',{'revision':rev-1,'id':ids[0],'aspect':1},error=True);check(generations()==before,'Rejected frame advanced cut')
        report['checks'].append('Legacy mode switches and each accepted frame advance only their actual viewport slot.')
        # Attach a hidden owned window only to queue captures; HostedViewport is
        # lazy and no draw/GPU operation is requested by this fixture.
        user32=ctypes.WinDLL('user32',use_last_error=True)
        user32.CreateWindowExW.argtypes=[ctypes.c_uint32,ctypes.c_wchar_p,ctypes.c_wchar_p,ctypes.c_uint32,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p]
        user32.CreateWindowExW.restype=ctypes.c_void_p
        user32.DestroyWindow.argtypes=[ctypes.c_void_p];user32.DestroyWindow.restype=ctypes.c_int
        parent=user32.CreateWindowExW(0,'STATIC','Poima camera cut owner',0,0,0,128,128,None,None,None,None);check(parent,'Hidden parent creation failed')
        window=user32.CreateWindowExW(0,'STATIC','Poima camera cut contract',0x40000000,0,0,128,128,parent,None,None,None);check(window,'Hidden child creation failed')
        lib.poima_desktop_attach.argtypes=[ctypes.c_void_p,ctypes.c_void_p];lib.poima_desktop_attach.restype=ctypes.c_int
        lib.poima_desktop_error.argtypes=[ctypes.c_void_p];lib.poima_desktop_error.restype=ctypes.c_char_p
        check(lib.poima_desktop_attach(host,window)==1,lib.poima_desktop_error(host))
        sid=uuid.uuid4().hex;rpc('runtime.start',{'revision':rev,'session_id':sid})
        storage=run/'saves';storage.mkdir()
        config=rpc('save.configure',{'request_id':uuid.uuid4().hex,'expected_generation':0,'root':str(storage)})['generation']
        rpc('save.write',{'request_id':uuid.uuid4().hex,'configuration_generation':config,'slot':'main','expected_generation':0,'session_id':sid,'expected_tick':0,'expected_gameplay_revision':0})
        queued=rpc('desktop.capture',{'revision':rev,'path':str(run/'stale.bmp')})
        check(queued['state']=='queued' and queued['presentation_source_id'],'Missing capture identity')
        rpc('save.load',{'request_id':uuid.uuid4().hex,'configuration_generation':config,'slot':'main','expected_generation':1,'revision':rev,'expected_session_id':sid,'expected_tick':0,'expected_gameplay_revision':0,'new_session_id':uuid.uuid4().hex})
        stale=rpc('desktop.capture.status',{'capture_id':queued['capture_id']})
        check(stale['state']=='error' and stale['error']['code']==-32009,'Same-tick replacement did not invalidate capture')
        check(not (run/'stale.bmp').exists(),'Stale capture wrote output')
        fresh=rpc('desktop.capture',{'revision':rev,'path':str(run/'fresh.bmp')})
        check(fresh['state']=='queued' and fresh['presentation_source_id']!=queued['presentation_source_id'],'Restored capture reused source identity')
        check(fresh['view_cut_generation']==queued['view_cut_generation'],'Runtime replacement changed independent view cut generation')
        rpc('desktop.camera',{'cut':True})
        cut=rpc('desktop.capture.status',{'capture_id':fresh['capture_id']})
        check(cut['state']=='error' and cut['error']['code']==-32009,'Explicit cut did not invalidate capture')
        report['checks'].append('Queued capture rejects actual same-tick save replacement into a fresh session and explicit cut; no stale file written.')

        report['passed']=True
    except BaseException:report['error']=traceback.format_exc()
    finally:
        if host and lib:
            try:lib.poima_desktop_destroy(host)
            except BaseException:report['passed']=False;report['cleanup_error']=traceback.format_exc()
        if window and user32:
            if not user32.DestroyWindow(window):report['passed']=False;report['window_cleanup_error']='DestroyWindow failed'
        if parent and user32:
            if not user32.DestroyWindow(parent):report['passed']=False;report['parent_cleanup_error']='DestroyWindow failed'
        if search:search.close()
        report['inputs_unchanged']=all(hashlib.sha256(v.read_bytes()).hexdigest()==report['input_sha256'][k]for k,v in paths.items())
        if not report['inputs_unchanged']:report['passed']=False
        (run/'evidence.json').write_text(json.dumps(report,indent=2)+'\n')
    check(report['passed'],str(run/'evidence.json'));print(json.dumps({'passed':True,'evidence':str(run/'evidence.json')}))
if __name__=='__main__':main()
