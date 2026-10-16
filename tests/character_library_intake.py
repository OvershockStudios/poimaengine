#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded original multi-take GLB intake; CPU-only, no downloads or conversion.

Supply unchanged KayKit Adventurers1.0 Knight.glb and its original CC0 license.
The licensed artwork is not embedded in this Apache-2.0 test. Original raw
accessors, curves, FK and skin math provide the independent oracle; cooked
geometry is matched to original triangle corners across tangent reindexing.
Windows execution requires an externally owned Job. No rendering or game build.
"""
from pathlib import Path
import argparse,bisect,copy,ctypes,hashlib,json,math,os,re,stat,struct,sys,time,traceback
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient,RpcError
from switchback_game_service import Budget,RetainedTransport
from imported_character_contract import SourceOracle,multiply,point,transform_matrix,normal_direction,interpolate_rotation
SOURCE_SHA='60428e3abc09ba83e595d256e3af8c5c976b46cdae599f0802fc82b4a3445168'
LICENSE_SHA='ae322141814056dda0deea7540d74c41d87aee1da319977cd1bd84ee5a923629'
SOURCE_URL='https://github.com/KayKit-Game-Assets/KayKit-Character-Pack-Adventures-1.0/tree/672074b73ba276876a19e8816ecdc5241817ab47'
MAX_CALLS=512;MAX_SECONDS=120;MIB=1024**2

def need(v,m):
    if not v:raise AssertionError(m)
def exact(a,b):
    if type(a)is not type(b):return False
    if type(a)is dict:return a.keys()==b.keys()and all(exact(a[k],b[k])for k in a)
    if type(a)is list:return len(a)==len(b)and all(exact(x,y)for x,y in zip(a,b))
    return a==b
def identity(b):return {'bytes':len(b),'sha256':hashlib.sha256(b).hexdigest()}
def raw(path,limit=64*MIB):
    s=path.lstat();need(stat.S_ISREG(s.st_mode)and s.st_nlink==1 and not getattr(s,'st_file_attributes',0)&0x400 and s.st_size<=limit,'Ordinary bounded input');return path.read_bytes()
def pin(path):return identity(raw(path))
def parsed(data):
    def pairs(rows):
        d={}
        for k,v in rows:need(k not in d,'Duplicate JSON field');d[k]=v
        return d
    def bad(_):raise AssertionError('Nonfinite JSON')
    return json.loads(data,object_pairs_hook=pairs,parse_constant=bad)
def near(a,b,tolerance,message):
    need(type(a)in(list,tuple)and type(b)in(list,tuple)and len(a)==len(b),'Exact vector/matrix length required')
    need(all(type(x)in(int,float)and math.isfinite(x)for x in a),'Finite actual numeric vector')
    error=max((abs(x-y)for x,y in zip(a,b)),default=0);need(error<=tolerance,message);return error


class LibraryOracle(SourceOracle):
    """Adapt the existing raw-accessor algorithms, not its pinned constructor."""
    def __init__(self,data):
        need(identity(data)=={'bytes':3659532,'sha256':SOURCE_SHA},'Exact original Knight.glb required')
        magic,version,total=struct.unpack_from('<III',data);need((magic,version,total)==(0x46546c67,2,len(data)),'Original GLB header')
        size,kind=struct.unpack_from('<II',data,12);need(kind==0x4e4f534a,'Original GLB JSON');self.document=parsed(data[20:20+size]);length,kind=struct.unpack_from('<II',data,20+size)
        need(kind==0x004e4942 and 28+size+length==len(data),'Original GLB binary');self.blob=data[28+size:];d=self.document;self.nodes=d['nodes'];self.parents={}
        for parent,node in enumerate(self.nodes):
            need('matrix'not in node,'Pinned original uses TRS')
            for child in node.get('children',[]):need(child not in self.parents,'Unique source parent');self.parents[child]=parent
        self.clips=[];keys=channels=0
        for i,clip in enumerate(d['animations']):
            curves=[]
            for ch in clip['channels']:
                sampler=clip['samplers'][ch['sampler']];need(sampler.get('interpolation','LINEAR')=='LINEAR','Pinned original LINEAR curves')
                times=[v[0]for v in self.accessor(sampler['input'])];values=self.accessor(sampler['output']);need(len(times)==len(values)>0,'Original time/value lengths')
                curves.append({'node':ch['target']['node'],'path':ch['target']['path'],'times':times,'values':values});keys+=len(times);channels+=1
            self.clips.append({'index':i,'name':clip['name'],'duration':max(c['times'][-1]for c in curves),'channels':curves})
        self.skin=d['skins'][0];self.binds=self.accessor(self.skin['inverseBindMatrices'])
        need((len(self.nodes),len(d['meshes']),len(d['skins']),len(self.skin['joints']),len(self.clips),channels,keys,len(d['materials']),len(d['images']))==(57,15,1,41,76,8712,90748,1,1),'Literal original library profile')
        self.meshes=[]
        for mesh in d['meshes']:
            need(len(mesh['primitives'])==1,'Pinned one primitive per mesh');p=mesh['primitives'][0];a=p['attributes'];self.meshes.append({'primitive':p,'positions':self.accessor(a['POSITION']),'normals':self.accessor(a['NORMAL']),'uv':self.accessor(a['TEXCOORD_0']),'joints':self.accessor(a['JOINTS_0'])if'JOINTS_0'in a else None,'weights':self.accessor(a['WEIGHTS_0'])if'WEIGHTS_0'in a else None,'indices':[r[0]for r in self.accessor(p['indices'])]})
        need(sum(len(m['positions'])for m in self.meshes)==7024 and sum(len(m['indices'])for m in self.meshes)==6952*3,'Original complete geometry cardinalities')
        need(sum('skin'in n for n in self.nodes if'mesh'in n)==6 and sum('skin'not in n for n in self.nodes if'mesh'in n)==9,'Mixed original rigid/skinned attachments')
        image=d['images'][0];view=d['bufferViews'][image['bufferView']];start=view.get('byteOffset',0);self.png=self.blob[start:start+view['byteLength']];need(len(self.png)==14172,'Original embedded atlas bytes');need(self.png[:8]==b'\x89PNG\r\n\x1a\n'and struct.unpack('>IIBBBBB',self.png[16:29])==(1024,1024,8,6,0,0,0),'Pinned original PNG header')
    def summaries(self):return [{'index':c['index'],'name':c['name'],'duration':c['duration'],'channels':len(c['channels'])}for c in self.clips]
    def pose(self,clip=None,time=0):
        local=[{'position':list(n.get('translation',[0,0,0])),'rotation':list(n.get('rotation',[0,0,0,1])),'scale':list(n.get('scale',[1,1,1]))}for n in self.nodes]
        if clip is not None:
            for c in self.clips[clip]['channels']:
                times=c['times'];right=min(len(times)-1,bisect.bisect_right(times,time));left=max(0,bisect.bisect_right(times,time)-1);u=0 if left==right else (time-times[left])/(times[right]-times[left]);a,b=c['values'][left],c['values'][right]
                value=interpolate_rotation(a,b,u)if c['path']=='rotation'else[x+(y-x)*u for x,y in zip(a,b)];local[c['node']][{'translation':'position','rotation':'rotation','scale':'scale'}[c['path']]]=list(value)
        mats=[transform_matrix(**n)for n in local];worlds={}
        def world(i):
            if i not in worlds:worlds[i]=multiply(world(self.parents[i]),mats[i])if i in self.parents else mats[i]
            return worlds[i]
        for i in range(57):world(i)
        return local,mats,[worlds[i]for i in range(57)]
    def vertex(self,pose,node,index):
        mesh=self.meshes[self.nodes[node]['mesh']];position=mesh['positions'][index];normal=mesh['normals'][index]
        if'skin'not in self.nodes[node]:return point(pose[2][node],position),normal_direction(pose[2][node],normal)
        joints,weights=mesh['joints'][index],mesh['weights'][index];total=sum(weights);weights=[x/total for x in weights]
        palette=[multiply(pose[2][joint],bind)for joint,bind in zip(self.skin['joints'],self.binds)];blend=[sum(weights[k]*palette[joints[k]][entry]for k in range(4))for entry in range(16)]
        return point(blend,position),normal_direction(blend,normal)

def decode_cooked(data,oracle):
    need(data[:8]==b'POIMAM04','Original animated package version unchanged');size,length=struct.unpack_from('<II',data,8);need(size+length+16==len(data)<=64*MIB,'Complete cooked lengths');meta=parsed(data[16:16+size]);blob=data[16+size:];cursor=0;meshes=[]
    need(len(meta['primitives'])==15 and len(meta['animations'])==76 and len(meta['skins'])==len(meta['images'])==1,'Full cooked library cardinalities')
    expected_skin={'name':oracle.skin.get('name',''),'skeleton':oracle.skin.get('skeleton',-1),'joints':oracle.skin['joints'],'inverse_bind':[list(m)for m in oracle.binds]};need(exact(meta['skins'][0],expected_skin),'Entire original skin name/palette/inverse-bind payload')
    need(exact(meta['roots'],oracle.document['scenes'][oracle.document.get('scene',0)]['nodes']),'Entire original selected scene roots')
    for mi,p in enumerate(meta['primitives']):
        source=oracle.meshes[mi]
        need(type(p['skinned'])is bool and p['skinned'] is(source['joints']is not None) and p['skinned'] is(source['weights']is not None),'Original primitive skin membership retained')
        count=p['vertices'];verts=[struct.unpack_from('<12f',blob,cursor+i*48)for i in range(count)];cursor+=count*48;influences=None
        if p['skinned']:influences=[(struct.unpack_from('<4I',blob,cursor+i*32),struct.unpack_from('<4f',blob,cursor+i*32+16))for i in range(count)];cursor+=count*32
        indices=struct.unpack_from('<'+'I'*p['indices'],blob,cursor);cursor+=p['indices']*4;need(len(indices)==len(source['indices']),'No triangle loss');mapping={}
        for si,ci in zip(source['indices'],indices):
            need(ci<count,'Cooked corner index');v=verts[ci];near(v[:3],source['positions'][si],0,'Original vertex position bytes');near(v[6:8],source['uv'][si],0,'Original UV bytes')
            n=source['normals'][si];length=math.sqrt(sum(x*x for x in n));near(v[3:6],[x/length for x in n],1e-7,'Normalized original normal')
            need(abs(sum(x*x for x in v[8:11])-1)<1e-4 and abs(sum(v[8+k]*v[3+k]for k in range(3)))<1e-4 and v[11]in(-1,1),'Valid cooked tangent frame')
            if influences is not None:
                j,w=influences[ci];need(tuple(source['joints'][si])==j,'Original joint palette indices');total=sum(source['weights'][si]);near(w,[x/total for x in source['weights'][si]],1e-7,'Original normalized skin weights')
            if ci in mapping:near(source['positions'][mapping[ci]],source['positions'][si],0,'Cooked seam correspondence')
            else:mapping[ci]=si
        need(set(mapping)==set(range(count)),'Every cooked vertex has an original corner witness')
        expected_material={'base_color':[1.0]*3,'emissive':[0.0]*3,'metallic':0.0,'roughness':0.5,'double_sided':True};need(exact(p['material'],expected_material),'Original opaque PBR factors')
        need(exact(p['textures'],[{'image':0,'wrap_s':10497,'wrap_t':10497,'min_filter':9987,'mag_filter':9729},None,None,None,None]),'Original atlas slot and sampler policy')
        meshes.append({'vertices':verts,'indices':indices,'mapping':mapping})
    need(cursor==meta['geometry_bytes'],'Complete geometry segment boundary')
    for expected,clip in zip(oracle.clips,meta['animations']):
        need(clip['name']==expected['name']and clip['duration']==expected['duration']and len(clip['channels'])==len(expected['channels']),'All original take metadata')
        for raw,channel in zip(expected['channels'],clip['channels']):
            keys=len(raw['times']);need(exact(channel,{'node':raw['node'],'path':{'translation':0,'rotation':1,'scale':2}[raw['path']],'interpolation':1,'keys':keys}),'All original channel metadata')
            times=struct.unpack_from('<'+'f'*keys,blob,cursor);cursor+=keys*4;need(tuple(raw['times'])==times,'Every original key time preserved')
            values=struct.unpack_from('<'+'f'*(4*keys),blob,cursor);cursor+=keys*16
            for i,wanted in enumerate(raw['values']):need(tuple(wanted)+(0.0,)*(4-len(wanted))==values[i*4:i*4+4],'Every original curve value preserved')
    need(cursor==meta['geometry_bytes']+meta['animation_bytes'],'Complete curve segment boundary')
    image=meta['images'][0];need(image['srgb']is True and len(image['mips'])==11,'Original atlas colorspace/mip count')
    mip_hashes=[];width=1024
    for level,mip in enumerate(image['mips']):
        need(exact(mip,{'width':width,'height':width,'bytes':width*width*4}),'Complete mip dimensions/bytes');pixels=blob[cursor:cursor+mip['bytes']];need(len(pixels)==mip['bytes'],'Complete retained mip payload')
        mip_hashes.append(identity(pixels));cursor+=mip['bytes'];width=max(1,width//2)
    need(cursor==len(blob),'No omitted/trailing cooked bytes')
    return meta,meshes,mip_hashes

class Owner:
    def __init__(self,binary,world,folder,report,deadline):
        folder.mkdir();self.raw=[];self.row={'pid':None,'closed':False,'calls':[]};report['owners'].append(self.row);self.report=report;self.deadline=deadline;self.world=world;self.close_attempted=False
        self.transport=RetainedTransport([str(binary),'world',str(world)],folder,Budget(deadline,[]),self.raw.append);self.row['pid']=self.transport.process.pid;self.client=WorldClient(self.transport)
    def call(self,method,params=None,error=None):
        need(sum(len(o['calls'])for o in self.report['owners'])<MAX_CALLS and time.monotonic()<self.deadline,'Whole512RPC/120s scope');request=copy.deepcopy(params or {});row={'method':method,'params':request};self.row['calls'].append(row)
        try:value=self.client.call(method,request,timeout=min(30,self.deadline-time.monotonic()))
        except RpcError as failed:
            row['error']=copy.deepcopy(self.raw[-1]['error']);need(error==failed.code,'Unexpected native rejection');return row['error']
        row['result']=copy.deepcopy(value);need(error is None and exact(value,self.raw[-1]['result']),'Whole successful native result');return value
    def page(self,method,params,total):
        result=[];offset=0
        for _ in range(256):
            value=self.call(method,dict(params,offset=offset,limit=64));count_key=params['section']if method=='asset.inspect'else'total';need(type(value[count_key])is int and value[count_key]==total,'Independent complete page cardinality in the actual method schema');rows=value['items'];need(type(rows)is list and len(rows)==min(64,total-offset),'Exact page cardinality');result+=rows;following=value['next_offset']
            if following is None:need(len(result)==total,'Complete page population');return result
            need(type(following)is int and following==offset+len(rows)and following>offset,'Bounded monotonic next offset');offset=following
        raise AssertionError('Pagination did not close')
    def state(self,label):
        # A real answered read-only request establishes native startup before
        # observing its operational lock; retain all three complete results.
        inspected=self.call('world.inspect');queried=self.call('entity.query');history=self.call('world.history')
        family={}
        for suffix in ('','.previous','.pending','.previous.pending'):
            path=Path(str(self.world)+suffix)
            try:path.lstat()
            except FileNotFoundError:family[suffix]=None
            else:family[suffix]=raw(path).hex()
        lock=Path(str(self.world)+'.lock');ls=lock.lstat();need(stat.S_ISREG(ls.st_mode)and ls.st_nlink==1 and ls.st_size==0 and not getattr(ls,'st_file_attributes',0)&0x400,'Owned ordinary zero-byte operational writer sidecar')
        lock_observation={'exists':True,'regular_file':True,'bytes':ls.st_size,'contents_read':False,'scope':'Operational writer sidecar metadata only while native owner is active; existence does not establish lock ownership.'}
        directory=Path(str(self.world)+'.assets');packages={}
        try:ds=directory.lstat()
        except FileNotFoundError:store_exists=False
        else:
            store_exists=True;need(stat.S_ISDIR(ds.st_mode)and not getattr(ds,'st_file_attributes',0)&0x400,'Ordinary unlinked asset-store directory')
        if store_exists:
            for path in sorted(directory.iterdir()):need(path.is_file(),'Only ordinary flat owned asset payloads');packages[path.name]=pin(path)
        result={'inspect':inspected,'query':queried,'history':history,'authored_files_hex':family,'operational_writer_lock':lock_observation,'store_directory_exists':store_exists,'packages':packages};self.report['state_observations'].append({'label':label,'value':result});return result
    def close(self):
        need(not self.close_attempted,'No repeat after unknown closure');self.close_attempted=True
        try:self.client.close()
        finally:self.row.update(exit_code=self.transport.returncode,closed=self.transport.closed,stderr=self.transport.stderr_tail,stderr_truncated=self.transport.stderr_truncated,stream_threads_closed=all(not thread.is_alive()for thread in self.transport.threads),faults=copy.deepcopy(self.transport.faults))
        need(self.client.cleanup_error is None and type(self.transport.returncode)is int and self.transport.returncode==0 and self.row['closed']and self.row['stream_threads_closed']and not self.row['stderr_truncated']and not self.row['faults'],'Natural clean native EOF/stream closure')
        stderr_bytes=raw(self.transport.paths['stderr'],32*MIB);pending=re.compile(rb'World publication retry pending: \[storage operation=MoveFileExW stage=(previous_publish|primary_publish) win32_code=([0-9]+) retry_attempts=1\]\r?\n');recovered=re.compile(rb'World publication recovered: \[storage operation=MoveFileExW stage=(previous_publish|primary_publish) retry_attempts=([0-9]+) first_win32_code=([0-9]+) last_win32_code=([0-9]+) scheduled_wait_ms=([0-9]+) retry_elapsed_us=([0-9]+)\]\r?\n');waiting=None;observed=[]
        for line in stderr_bytes.splitlines(keepends=True):
            first=pending.fullmatch(line);last=recovered.fullmatch(line)
            if first:
                need(waiting is None and int(first[2])in(5,32,33),'Actual first eligible publication failure');waiting=first.groups();observed.append({'event':'pending','stage':first[1].decode(),'actual_failed_code':int(first[2])})
            else:
                need(last is not None and waiting is not None and last[1]==waiting[0]and int(last[3])==int(waiting[1])and int(last[4])in(5,32,33)and 2<=int(last[2])<=6 and int(last[5])==[0,2,6,14,30,62][int(last[2])-1],'Only complete literal successful retry diagnostics are permitted');waiting=None;observed.append({'event':'recovered','stage':last[1].decode(),'attempts':int(last[2]),'first_failed_code':int(last[3]),'last_failed_code':int(last[4]),'scheduled_wait_ms':int(last[5]),'elapsed_us':int(last[6])})
        need(waiting is None,'Pending publication observation lacks a successful recovery');self.row['publication_diagnostics']=observed
        # The process and both retained readers have closed above. Only now may
        # Windows open this formerly share-zero operational file for content.
        lock=Path(str(self.world)+'.lock');lock_bytes=raw(lock);need(lock_bytes==b'','Actual post-EOF writer sidecar is not empty');self.row['operational_writer_lock_after_eof']={'exists':True,'identity':identity(lock_bytes),'observed_after_native_exit_code':self.transport.returncode,'scope':'Post-EOF ordinary file observation, separate from the four authoritative authored payloads.'}

def observe_pose(owner,asset,oracle,cooked,clip,time,label,report,source=None):
    expected=oracle.pose(clip,time);params={'asset':asset,'time':time,'loop':False,'section':'nodes'}
    if clip is not None:params['clip']=clip
    rows=owner.page('asset.animation.sample',params,57);matrix_error=0
    for i,row in enumerate(rows):
        need(row['index']==i and type(row['index'])is int and row['parent']==oracle.parents.get(i,-1),'Original ordered node identity')
        matrix_error=max(matrix_error,near(transform_matrix(row['position'],row['rotation'],row['scale']),expected[1][i],2e-5,'Independent raw local pose'),near(row['world'],expected[2][i],2e-5,'Independent raw hierarchy FK'))
    if source is not None:
        selector={'kind':'rest'}if clip is None else{'kind':'sample','clip':clip,'time':time}
        original=owner.page('asset.source.inspect',{'source':str(source),'section':'nodes','pose':selector,'expected_model_sha256':asset},57)
        for i,row in enumerate(original):near(row['world'],expected[2][i],2e-5,'Original source pose FK')
    vertex_error=normal_error=0
    for node in (0,52):
        mi=oracle.nodes[node]['mesh'];vertices=owner.call('asset.animation.sample',dict(params,section='vertices',node=node,primitive=mi,offset=0,limit=8));need(len(vertices['items'])==8,'Bounded original vertex page')
        for i,v in enumerate(vertices['items']):
            need(type(v['index'])is int and v['index']==i,'Sampled vertex identity');si=cooked[mi]['mapping'][i];wanted,normal=oracle.vertex(expected,node,si)
            vertex_error=max(vertex_error,near(v['world_position'],wanted,5e-5,'Original independent raw skin/rigid vertex'))
            normal_error=max(normal_error,near(normal_direction(expected[2][node],v['normal']),normal,1e-4,'Original independent inverse-transpose normal'))
    report['pose_checks'].append({'label':label,'clip':clip,'time':time,'all_nodes':57,'mixed_mesh_nodes':[0,52],'vertices':16,'maximum_matrix_error':matrix_error,'maximum_vertex_error':vertex_error,'maximum_normal_error':normal_error})
    return rows

def census(directory):
    count=total=0
    for path in directory.rglob('*'):
        need(not path.is_symlink(),'No linked output');s=path.lstat()
        if stat.S_ISDIR(s.st_mode):continue
        need(stat.S_ISREG(s.st_mode)and s.st_nlink==1 and not getattr(s,'st_file_attributes',0)&0x400,'Ordinary retained output');count+=1;total+=s.st_size;need(count<=512 and total<=120*MIB,'Reserve8MiB for final report under128MiB')
    return {'files':count,'bytes':total}

def legacy_package_parity(current,legacy,output,report,deadline):
    source=ROOT/'examples/assets/animated-ribbon.glb';need(source.exists(),'Existing accepted original ribbon source');outputs=[]
    for index,binary in enumerate((legacy,current)):
        directory=output/('accepted-parity-'+str(index));directory.mkdir();world=directory/'world.json';owner=None
        try:
            owner=Owner(binary,world,directory/'owner',report,deadline);before=owner.state('accepted-parity-before-'+str(index));inspected=owner.call('asset.source.inspect',{'source':str(source)});accepted=owner.call('asset.import',{'source':str(source)});asset=accepted['asset'];package=Path(str(world)+'.assets')/(asset+'.pmodel');bytes_=raw(package)
            need(identity(bytes_)['sha256']==asset==inspected['model_sha256'],'Accepted old-content whole hash identity');expected=copy.deepcopy(before);expected['store_directory_exists']=True;expected['packages']={asset+'.pmodel':identity(bytes_)};need(exact(owner.state('accepted-parity-after-'+str(index)),expected),'Accepted parity changed full native/authoring state')
            outputs.append({'source_inspection':inspected,'import_result':accepted,'package_inventory':expected['packages'],'whole_package_bytes':bytes_});owner.close();owner=None
        finally:
            if owner is not None and not owner.close_attempted:owner.close()
    need(exact(outputs[0]['source_inspection'],outputs[1]['source_inspection'])and exact(outputs[0]['import_result'],outputs[1]['import_result'])and exact(outputs[0]['package_inventory'],outputs[1]['package_inventory'])and outputs[0]['whole_package_bytes']==outputs[1]['whole_package_bytes'],'Accepted173/174 full result and whole package bytes changed')
    return {'passed':True,'owners':2,'whole_package_pairs':1,'complete_results_compared':2,'package_identity':identity(outputs[0]['whole_package_bytes']),'source_identity':pin(source),'scope':'Same unchanged existing animated-ribbon GLB on separately supplied executables; complete source/import results and all package bytes, not a claim that old173 reads new over8192-channel packages.'}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--binary',type=Path,required=True);p.add_argument('--legacy-binary',type=Path);p.add_argument('--source',type=Path,required=True);p.add_argument('--license',type=Path,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args();args.output=args.output.resolve();args.source=args.source.resolve();args.license=args.license.resolve();start=time.monotonic();deadline=start+MAX_SECONDS
    need(not sys.flags.optimize and not args.output.exists(),'Nonoptimized Python/fresh output')
    if os.name=='nt':
        kernel=ctypes.WinDLL('kernel32',use_last_error=True);kernel.GetCurrentProcess.restype=ctypes.c_void_p;kernel.IsProcessInJob.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.POINTER(ctypes.c_int)];flag=ctypes.c_int();need(kernel.IsProcessInJob(kernel.GetCurrentProcess(),None,ctypes.byref(flag))and flag.value,'Caller-owned Windows Job required')
    source=raw(args.source);license_bytes=raw(args.license);need(hashlib.sha256(license_bytes).hexdigest()==LICENSE_SHA,'Original unchanged KayKit CC0 license');oracle=LibraryOracle(source)
    args.output.mkdir(parents=True);sources=args.output/'sources';sources.mkdir();copy_source=sources/'Knight.glb';copy_source.write_bytes(source);(sources/'LICENSE.txt').write_bytes(license_bytes);binary=args.binary.resolve();world=args.output/'world.json';owner=None
    helpers=[Path(__file__).resolve(),ROOT/'tests/imported_character_contract.py',ROOT/'tests/scene_capture.py',ROOT/'tests/switchback_game_service.py',ROOT/'tests/switchback_oracle.py']+sorted(p for p in(ROOT/'tools/python/poima_client').rglob('*')if p.suffix in('.py','.json')and'__pycache__'not in p.parts)
    pins={str(p):pin(p)for p in helpers};originals={str(args.source.resolve()):identity(source),str(args.license.resolve()):identity(license_bytes)};binary_pin=pin(binary);legacy_pin=pin(args.legacy_binary.resolve())if args.legacy_binary else None
    if args.legacy_binary:pins[str(ROOT/'examples/assets/animated-ribbon.glb')]=pin(ROOT/'examples/assets/animated-ribbon.glb')
    report={'format':'poima.character-library-intake.v1','passed':False,'errors':[],'owners':[],'state_observations':[],'pose_checks':[],'source_inputs':pins,'original_inputs':originals,'binary_input':binary_pin,'legacy_binary_input':legacy_pin,'attribution':{'creator':'Kay Lousberg','pack':'KayKit Adventurers1.0','license':'CC0-1.0','source':SOURCE_URL},'scope':'Original CPU multi-take geometry/material/skin intake and independent raw-curve/FK/skinning checks. No renderer, general retargeting, editor, performance, game completion or broad exporter qualification.'}
    try:
        if args.legacy_binary:report['accepted_legacy_parity']=legacy_package_parity(binary,args.legacy_binary.resolve(),args.output,report,deadline)
        owner=Owner(binary,world,args.output/'warm',report,deadline);seed_request={'request_id':'a'*32,'base_revision':0,'ops':[{'op':'entity.create','id':'1'*32,'name':'Retained original-character intake'}]};seed_receipt=owner.call('world.transact',seed_request);before=owner.state('before-source-inspection')
        info=owner.call('asset.source.inspect',{'source':str(copy_source)});need((info['nodes'],info['primitives'],info['skins'],info['animations'],info['images'],info['triangles'])==(57,15,1,76,1,6952)and info['published']is False,'Original full source summary')
        original_clips=owner.page('asset.source.inspect',{'source':str(copy_source),'section':'animations','expected_model_sha256':info['model_sha256']},76);need(exact(original_clips,oracle.summaries()),'All76 independently original take summaries')
        need(exact(owner.state('after-source-inspection'),before),'Source inspection changed complete authoring/history/package state')
        imported=owner.call('asset.import',{'source':str(copy_source)});asset=imported['asset'];need(asset==info['model_sha256']and imported['nodes']==57 and imported['animations']==76 and imported['skins']==1 and imported['primitives']==15,'Original same-model fingerprint and import profile');report['imported']=imported
        package=Path(str(world)+'.assets')/(asset+'.pmodel');cooked_bytes=raw(package);need(identity(cooked_bytes)['sha256']==asset,'Full content-addressed cooked identity');meta,cooked,mips=decode_cooked(cooked_bytes,oracle);(args.output/'original-cooked.pmodel').write_bytes(cooked_bytes);report['independent_cooked_checks']={'geometry_triangles':6952,'original_source_vertices':7024,'retained_cooked_vertices':sum(len(m['vertices'])for m in cooked),'all_takes':76,'all_channels':8712,'all_keys':90748,'image_mip_identities':mips,'original_embedded_png':identity(oracle.png),'original_png_width':1024,'original_png_height':1024,'scope':'All original triangle corners/skin attributes and all curve arrays; original PNG bytes/header retained. Cooked RGBA+mip payloads retain identities and dimensions, without an independent texel/filtering conformance claim.'}
        expected=copy.deepcopy(before);expected['store_directory_exists']=True;expected['packages']={asset+'.pmodel':identity(cooked_bytes)};after=owner.state('after-import');need(exact(after,expected),'Only the independently validated package may be published')
        nodes=owner.page('asset.inspect',{'asset':asset,'section':'nodes'},57);clips=owner.page('asset.inspect',{'asset':asset,'section':'animations'},76);need(exact(clips,oracle.summaries()),'All76 cooked take summaries')
        for i,row in enumerate(nodes):need(type(row['index'])is type(row['parent'])is type(row['skin'])is int and row['index']==i and row['name']==oracle.nodes[i]['name']and row['parent']==oracle.parents.get(i,-1)and row['skin']==oracle.nodes[i].get('skin',-1)and row['primitives']==([oracle.nodes[i]['mesh']]if'mesh'in oracle.nodes[i]else[]),'Every original mixed attachment/node')
        binds=owner.page('asset.animation.skin',{'asset':asset,'skin':0},41)
        for i,row in enumerate(binds):need(row['index']==i and row['node']==oracle.skin['joints'][i],'Complete original ordered skin palette');near(row['inverse_bind'],oracle.binds[i],0,'Original inverse-bind bytes')
        schedule=[(None,0,'rest')]
        for ci in (0,75):
            curve=max(oracle.clips[ci]['channels'],key=lambda c:len(c['times']));times=curve['times'];k=max(1,len(times)//2);need(k<len(times),'Nontrivial selected original curve');schedule.append((ci,times[k],'clip'+str(ci)+'-exact-key'));schedule.append((ci,(times[k-1]+times[k])/2,'clip'+str(ci)+'-mid-key'))
        for ci,t,label in schedule:observe_pose(owner,asset,oracle,cooked,ci,t,label,report,copy_source)
        unchanged=owner.state('before-known-rejections')
        stale=owner.call('asset.source.inspect',{'source':str(copy_source),'expected_model_sha256':'0'*64},error=-32050);need(stale['message']=='Source model fingerprint changed; inspect the current source before selecting nodes or takes.','Exact stale original fingerprint error');need(exact(owner.state('after-stale-fingerprint'),unchanged),'Stale source guard changed full state')
        copy_source.write_bytes(source[:-1]);owner.call('asset.import',{'source':str(copy_source)},error=-32050);need(exact(owner.state('after-truncated-owned-source'),unchanged),'Rejected corrupt owned source changed full state');copy_source.write_bytes(source);need(owner.call('asset.import',{'source':str(copy_source)})['asset']==asset,'Identical source request repair changed identity')
        corrupted=cooked_bytes[:-1]+bytes([cooked_bytes[-1]^1]);package.write_bytes(corrupted);bad=owner.state('before-corrupt-package-observation');owner.call('asset.inspect',{'asset':asset},error=-32050);need(exact(owner.state('after-corrupt-package-observation'),bad),'Rejected corrupt cooked asset changed full state');package.write_bytes(cooked_bytes);need(exact(owner.state('restored-original-package'),unchanged),'Repair did not restore exact whole state')
        warm=owner.call('asset.inspect',{'asset':asset});owner.close();owner=None;sources.rename(args.output/'sources-retained-away');need(not copy_source.exists(),'Original admitted source path must be absent for cold owner')
        # Relocate the complete owned authored payload family and package store.
        # The unlocked operational lock is retained at its old path; the cold
        # native owner must create/acquire its own lock beside the relocated world.
        prior_world=world;prior_store=Path(str(world)+'.assets');relocated=args.output/'relocated';relocated.mkdir();world=relocated/'world.json'
        moved=[]
        for suffix in ('','.previous','.pending','.previous.pending'):
            old=Path(str(prior_world)+suffix)
            if old.exists():old.rename(Path(str(world)+suffix));moved.append(suffix)
            need(not old.exists(),'Old authoritative world payload path must be absent')
        prior_store.rename(Path(str(world)+'.assets'));package=Path(str(world)+'.assets')/(asset+'.pmodel')
        need(not prior_world.exists()and not prior_store.exists()and not copy_source.exists(),'Old admitted world/store/source paths remain unavailable')
        report['relocation']={'moved_authoritative_payload_suffixes':moved,'old_world_path_absent':True,'old_store_path_absent':True,'admitted_source_path_absent':True,'caller_original_inputs_retained':True,'scope':'Fresh native owner reads relocated owned world and all cooked assets; admitted source copy is retained away at a different path. Original caller source/license stay unchanged; not a claim that every source copy is globally absent. Operational writer lock is separate from authored payloads.'}
        owner=Owner(binary,world,args.output/'cold',report,deadline);need(exact(owner.call('asset.inspect',{'asset':asset}),warm),'Source-absent full cooked summary changed');cold=owner.state('cold-original-source-absent');cold_expected=copy.deepcopy(unchanged);cold_expected['history'].update(undo_count=0,redo_count=0,bytes=0);need(exact(cold,cold_expected),'Cold complete world differs except declared session-local undo reset')
        expected_seed_replay=copy.deepcopy(seed_receipt);need(expected_seed_replay['replayed']is False,'Original seed receipt must be a first commit');expected_seed_replay['replayed']=True;report['cold_seed_receipt_forecast']=copy.deepcopy(expected_seed_replay)
        need(exact(owner.call('world.transact',seed_request),expected_seed_replay),'Cold original persisted request receipt differs from exact replay forecast');need(exact(owner.state('cold-persisted-receipt-replay'),cold),'Cold retry committed twice or changed full state')
        ci,t,label=schedule[-1];observe_pose(owner,asset,oracle,cooked,ci,t,'cold-'+label,report);need(pin(package)==identity(cooked_bytes),'Source-absent cooked bytes changed');owner.close();owner=None
        need(all(pin(Path(name))==value for name,value in originals.items()),'Caller original content changed');need(pin(binary)==binary_pin and (not args.legacy_binary or pin(args.legacy_binary.resolve())==legacy_pin)and all(pin(Path(name))==value for name,value in pins.items()),'Consumed executable/helper sources changed');report.update(passed=True,source_inputs_unchanged=True,binary_input_unchanged=True,original_inputs_unchanged=True)
    except BaseException:report['errors'].append(traceback.format_exc())
    finally:
        if owner is not None and not owner.close_attempted:
            try:owner.close()
            except BaseException:report['errors'].append(traceback.format_exc())
        report['seconds']=time.monotonic()-start;report['passed']=bool(report['passed']and not report['errors']and report['seconds']<MAX_SECONDS and len(report['owners'])==(4 if args.legacy_binary else 2) and all(o.get('closed')and o.get('exit_code')==0 for o in report['owners']))
        report['output_before_report']=census(args.output);encoded=(json.dumps(report,indent=2,allow_nan=False)+'\n').encode();need(len(encoded)<=8*MIB,'Report8MiB reserve')
        with(args.output/'evidence.json').open('xb')as f:f.write(encoded)
    print(json.dumps({'passed':report['passed'],'errors':report['errors'],'seconds':report['seconds']}));return 0 if report['passed']else 1
if __name__=='__main__':raise SystemExit(main())
