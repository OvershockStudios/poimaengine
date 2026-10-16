#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Opt-in FBX material witnesses, legacy parity and atomic rejection/recovery.

Original parameterized ASCII/binary fixtures only; no downloaded artwork,
compiler, renderer or source application. Native Windows requires an external
owned Job. Original request/reply bytes are retained. An optional unchanged
--legacy-binary repeats omitted-flag calls for complete old-error/result parity.
"""
import argparse
import copy
import ctypes
import hashlib
import json
from pathlib import Path
import stat
import sys
import time
import traceback
import uuid

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/python'))
from poima_client import WorldClient,RpcError
from switchback_game_service import Budget,RetainedTransport
import fbx_fixture as fixture
from texture_fixture import png


def need(value,message):
    if not value:raise AssertionError(message)


def exact(a,b):
    if type(a)is not type(b):return False
    if type(a)is dict:return a.keys()==b.keys()and all(exact(a[key],b[key])for key in a)
    if type(a)is list:return len(a)==len(b)and all(exact(a[i],b[i])for i in range(len(a)))
    return a==b


def identity(path):
    raw=Path(path).read_bytes();return dict(bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest())


def package_tree(world):
    root=Path(str(world)+'.assets');result={}
    if not root.exists():return result
    for path in sorted(root.rglob('*')):
        row=path.lstat();need(not stat.S_ISLNK(row.st_mode)and not getattr(row,'st_file_attributes',0)&0x400,'Unexpected package link')
        if stat.S_ISDIR(row.st_mode):continue
        need(stat.S_ISREG(row.st_mode),'Unexpected package object');result[path.relative_to(root).as_posix()]=identity(path)
    return result


def census(directory):
    count=total=0
    for path in directory.rglob('*'):
        row=path.lstat()
        need(not stat.S_ISLNK(row.st_mode)and not getattr(row,'st_file_attributes',0)&0x400,'Unexpected output link')
        if stat.S_ISDIR(row.st_mode):continue
        need(stat.S_ISREG(row.st_mode),'Unexpected nonordinary output')
        count+=1;total+=row.st_size
        need(count<=4095 and total<=120*1024**2,'Output exceeds reservation for final evidence')
    return dict(files=count,bytes=total)


def field_raw(text,key):
    """Extract exact JSON member bytes, preserving absent-data distinctions."""
    decoder=json.JSONDecoder();i=0
    while text[i].isspace():i+=1
    need(text[i]=='{','Raw object expected');i+=1
    while True:
        while text[i].isspace():i+=1
        if text[i]=='}':break
        name,end=decoder.raw_decode(text,i);i=end
        while text[i].isspace():i+=1
        need(text[i]==':','Raw colon');i+=1
        while text[i].isspace():i+=1
        start=i;_,end=decoder.raw_decode(text,i)
        if name==key:return text[start:end]
        i=end
        while text[i].isspace():i+=1
        if text[i]=='}':break
        need(text[i]==',','Raw comma');i+=1
    raise AssertionError('Raw member absent')


def material(identity=700,name='RejectedMaterial',opacity=0):
    return (f'Material: {identity}, "Material::{name}", "" {{\nVersion: 102\nShadingModel: "lambert"\nProperties70: {{\n'
        'P: "DiffuseColor", "Color", "", "A", 1,1,1\n'
        f'P: "TransparencyFactor", "Number", "", "A", {opacity}\n}}\n}}\n')


def sources():
    baseline=fixture.textured(skin=True,takes=('Move',))
    opacity=baseline.replace('"TransparencyFactor", "Number", "", "A", 0','"TransparencyFactor", "Number", "", "A", .5')
    normal_good=fixture.textured(skin=True,takes=('Move',),normal=True)
    normal_bad=normal_good.replace('    P: "DiffuseColor"','    P: "NormalMap", "Vector3D", "", "A", 1,2,3\n    P: "DiffuseColor"')
    scalar_bad=normal_good.replace('    P: "DiffuseColor"','    P: "NormalMap", "Number", "", "A", .5\n    P: "DiffuseColor"')
    uv_bad=baseline.replace('P: "UVSet", "KString", "", "", "UVMap"','P: "UVSet", "KString", "", "", "MissingUV"')
    unknown=baseline.replace('ShadingModel: "lambert"','ShadingModel: "PoimaUnknownShader"')
    # Two original faces select different NODE-LOCAL material slots. The first
    # material is valid; slot1 is the witness, not an assumed global index.
    two=baseline.replace(fixture.array('PolygonVertexIndex',[0,1,-3]),fixture.array('PolygonVertexIndex',[0,1,-3,0,1,-3]))
    two=two.replace(fixture.array('Normals',[0,0,1]*3),fixture.array('Normals',[0,0,1]*6))
    two=two.replace(fixture.array('UV',[.25,.25,.75,.25,.25,.75]),fixture.array('UV',[.25,.25,.75,.25,.25,.75]*2))
    two=two.replace('MappingInformationType: "AllSame"','MappingInformationType: "ByPolygon"').replace(fixture.array('Materials',[0]),fixture.array('Materials',[0,1]))
    two=two.replace('Objects: {\n','Objects: {\n'+material()).replace('C: "OO",600,100\n','C: "OO",600,100\nC: "OO",700,100\n')
    two_bad=two.replace('P: "TransparencyFactor", "Number", "", "A", 0\n','P: "TransparencyFactor", "Number", "", "A", .5\n',1)
    # Geometry is shared by two static nodes. Only the second node's material
    # rejects, proving the witness follows the actual owner rather than mesh0.
    second=fixture.textured().replace('Objects: {\n','Objects: {\n'+material()+fixture.model(900,'SecondMesh','Mesh'))
    second=second.replace('Connections: {\n','Connections: {\nC: "OO",900,0\nC: "OO",200,900\nC: "OO",700,900\n')
    second_bad=second.replace('P: "TransparencyFactor", "Number", "", "A", 0\n','P: "TransparencyFactor", "Number", "", "A", .5\n',1)
    # These GLTF-class material properties are direct mappings in the vendored
    # ufbx shader table, not guessed exporter conventions. No geometry changes.
    class_properties=(f'    P: "3dsMax|ClassIDa", "int", "Integer", "", {0x38420192}\n'
        f'    P: "3dsMax|ClassIDb", "int", "Integer", "", {0x45fe4e1b}\n')
    gltf=baseline.replace('ShadingModel: "lambert"','ShadingModel: "PoimaAnalyticGLTF"').replace('    P: "DiffuseColor"',class_properties+'    P: "DiffuseColor"')
    def extension(text,name,value):
        return text.replace('    P: "DiffuseColor"',f'    P: "3dsMax|extension|{name}", "Number", "", "A", {value}\n    P: "DiffuseColor"')
    transmission=extension(gltf,'transmission',.5);transmission_good=extension(gltf,'transmission',0)
    unlit=extension(gltf,'unlit',1);unlit_good=extension(gltf,'unlit',0)
    bump=baseline.replace('C: "OP",610,600,"DiffuseColor"','C: "OP",610,600,"Bump"')
    # Remove the complete UV definition and its Layer reference, leaving a valid
    # untextured geometry representation. Its still-connected named texture is
    # what rejects, with an actual observed UV-set count of zero.
    uv_start=baseline.index('LayerElementUV: 0 {');uv_end=baseline.index('LayerElementMaterial: 0 {',uv_start)
    zero_uv=(baseline[:uv_start]+baseline[uv_end:]).replace('LayerElement: { Type: "LayerElementUV"\nTypedIndex: 0 }\n','')
    # Material names are display data, unlike node names (which reject earlier
    # over 256 bytes). This 321-byte name cuts a UTF-8 sequence at byte256; the
    # service's existing repair/boundary logic yields exactly255 valid bytes.
    long_name='M'+'é'*160;shown_long_name='M'+'é'*127
    long_good=baseline.replace('Material::Analytic','Material::'+long_name)
    long_bad=opacity.replace('Material::Analytic','Material::'+long_name)
    return [
        dict(label='advanced-transmission',bad=transmission,good=transmission_good,reason='advanced_feature',feature='pbr.transmission_factor',node='Mesh',slot=0,material='Analytic',observed=None,limit=None),
        dict(label='unlit',bad=unlit,good=unlit_good,reason='unlit',feature='unlit',node='Mesh',slot=0,material='Analytic',observed=None,limit=None),
        dict(label='bump-slot',bad=bump,good=baseline,reason='texture_slot',feature='fbx.bump',node='Mesh',slot=0,material='Analytic',observed=None,limit=None),
        dict(label='zero-uv',bad=zero_uv,good=baseline,reason='uv_binding',feature='texture.uv_set',node='Mesh',slot=0,material='Analytic',observed=0,limit=1),
        dict(label='long-material-name',bad=long_bad,good=long_good,reason='transparency',feature='opacity_or_transparency',node='Mesh',slot=0,material=shown_long_name,observed=None,limit=None),
        dict(label='opacity',bad=opacity,good=baseline,reason='transparency',feature='opacity_or_transparency',node='Mesh',slot=0,material='Analytic',observed=None,limit=None),
        dict(label='normal-vector',bad=normal_bad,good=normal_good,reason='normal_response',feature='normal_map.response',node='Mesh',slot=0,material='Analytic',observed=3,limit=1),
        dict(label='normal-scalar',bad=scalar_bad,good=normal_good,reason='normal_response',feature='normal_map.response',node='Mesh',slot=0,material='Analytic',observed=1,limit=1),
        dict(label='uv-name',bad=uv_bad,good=baseline,reason='uv_binding',feature='texture.uv_set',node='Mesh',slot=0,material='Analytic',observed=None,limit=None),
        dict(label='shader',bad=unknown,good=baseline,reason='shader_model',feature='shader_model',node='Mesh',slot=0,material='Analytic',observed=None,limit=None),
        dict(label='slot-one',bad=two_bad,good=two,reason='transparency',feature='opacity_or_transparency',node='Mesh',slot=1,material='RejectedMaterial',observed=None,limit=None),
        dict(label='second-node',bad=second_bad,good=second,reason='transparency',feature='opacity_or_transparency',node='SecondMesh',slot=0,material='RejectedMaterial',observed=None,limit=None)]


class Owner:
    def __init__(self,binary,directory,deadline,record):
        self.directory=directory;directory.mkdir();self.world=directory/'world.json';self.record=record
        self.responses=[];self.rows=[];self.budget=Budget(deadline,[])
        self.transport=RetainedTransport([str(binary),'world',str(self.world)],directory,self.budget,self.responses.append)
        self.client=WorldClient(self.transport);self.record.update(pid=self.transport.process.pid,calls=self.rows,raw={key:str(value)for key,value in self.transport.paths.items()})
        self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=0,ops=[dict(op='entity.create',id='1'*32,name='Preserved authored object')]))
    def call(self,method,params=None,error=None):
        need(len(self.rows)<1200,'Bounded protocol call population')
        row=dict(method=method,params=copy.deepcopy(params or {}));self.rows.append(row)
        try:
            result=self.client.call(method,row['params'],timeout=self.budget.remaining());need(error is None,'Expected rejection');row['result']=result;return result
        except RpcError as failure:
            need(error==failure.code,'Unexpected native error');original=self.responses[-1]
            need(original['id']==self.transport.counter and set(original)=={'jsonrpc','id','error'},'Original error envelope')
            row['error']=copy.deepcopy(original['error'])
            # Pump flushes retained bytes before queueing this original frame.
            line=self.transport.paths['stdout'].read_bytes().splitlines()[-1].decode('utf-8')
            need(exact(json.loads(line),original),'Original retained response bytes')
            row['raw_error']=field_raw(line,'error');return row['error']
    def state(self):
        return dict(world=self.call('world.inspect'),history=self.call('world.history'),world_bytes=self.world.read_bytes().hex(),packages=package_tree(self.world))
    def rejected(self,method,params,code=-32050):
        before=self.state();result=self.call(method,params,error=code);row=self.rows[-1]
        need(exact(before,self.state()),'Rejected request changed complete authored/history/file/package observations')
        return result,row
    def close(self):
        self.client.close();self.record.update(exit_code=self.transport.returncode,closed=self.client.closed,stderr=self.transport.stderr_tail)
        need(self.client.closed and self.transport.returncode==0 and not self.transport.stderr_tail,'Actual native closure/diagnostics')


def run(args):
    need(not args.output.exists(),'Fresh output only');args.output.mkdir(parents=True)
    if sys.platform=='win32':
        kernel=ctypes.WinDLL('kernel32',use_last_error=True);kernel.GetCurrentProcess.restype=ctypes.c_void_p
        inside=ctypes.c_int();kernel.IsProcessInJob.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.POINTER(ctypes.c_int)]
        need(kernel.IsProcessInJob(kernel.GetCurrentProcess(),None,ctypes.byref(inside))and inside.value,'External Windows Job required')
    started=time.monotonic();deadline=started+180
    record=dict(passed=False,owners=[],cases=[],legacy_pairs=[],errors=[],scope='Opt-in material diagnostics and old-request parity, not broader character/export/rendering support.')
    owner=None;legacy_requests=[];source_root=args.output/'sources';source_root.mkdir()
    def retain_legacy(method,params,result,row=None):
        legacy_requests.append(dict(method=method,params=copy.deepcopy(params),result=copy.deepcopy(result),raw_error=None if row is None else row.get('raw_error')))
    try:
        owner_record={};record['owners'].append(owner_record);owner=Owner(args.binary.resolve(),args.output/'candidate',deadline,owner_record)
        for method in('asset.import','asset.source.inspect'):
            discovery=owner.call('world.describe',dict(view='method',name=method));schema=discovery['methods'][method]
            need(schema['properties']['fbx_profile_diagnostics']==dict(type='boolean',default=False),'Actual opt-in discovery/default')
            need('fbx_material_profile'in schema['x-error-data'],'Structured error discovery missing')
        for encoding in('ascii','binary'):
            for number,case in enumerate(sources()):
                folder=source_root/(encoding+'-'+str(number));folder.mkdir()
                pixels=[channel for y in range(2)for x in range(2)for channel in(x*255,y*255,(x^y)*255,255)]
                (folder/'color.png').write_bytes(png(2,2,pixels));(folder/'normal.png').write_bytes(png(2,2,[c for _ in range(4)for c in(128,128,255,255)]))
                encode=lambda text:text.encode('utf-8')if encoding=='ascii'else fixture.binary_encode(text)
                bad=folder/'bad.fbx';good=folder/'good.fbx';need(case['bad']!=case['good'],'Original procedural rejection must differ from repair')
                bad.write_bytes(encode(case['bad']));good.write_bytes(encode(case['good']))
                nodes=owner.call('asset.source.inspect',dict(source=str(good),section='nodes',limit=64))
                matches=[item['index']for item in nodes['items']if item['name']==case['node']]
                need(nodes['next_offset']is None and len(matches)==1,'Independent repaired hierarchy identifies exactly one source owner')
                expected=dict(type='fbx_material_profile',version=1,reason=case['reason'],feature=case['feature'],node=matches[0],node_name=case['node'],material_slot=case['slot'],material_name=case['material'],observed_count=case['observed'],limit=case['limit'],source_role='base',donor_index=None)
                for method,role in(('asset.import','base'),('asset.source.inspect','inspection')):
                    params=dict(source=str(bad));default,row=owner.rejected(method,params);retain_legacy(method,params,default,row)
                    need(set(default)=={'code','message'},'Legacy omitted material error gained fields')
                    disabled,off=owner.rejected(method,dict(params,fbx_profile_diagnostics=False))
                    need(exact(disabled,default)and off['raw_error']==row['raw_error'],'Explicit false changed complete raw legacy error')
                    enabled,on=owner.rejected(method,dict(params,fbx_profile_diagnostics=True));wanted=copy.deepcopy(expected);wanted['source_role']=role
                    need(len(enabled['data']['node_name'].encode('utf-8'))<=256 and len(enabled['data']['material_name'].encode('utf-8'))<=256,'Material display witnesses exceed actual byte bounds')
                    need(exact(enabled,dict(code=default['code'],message=default['message'],data=wanted)),'Exact opted-in source material witness')
                    need(str(folder)not in on['raw_error'],'Opt-in witness leaked resolved source directory')
                for method in('asset.import','asset.source.inspect'):
                    params=dict(source=str(good));baseline=owner.call(method,params);retain_legacy(method,params,baseline)
                    for flag in(False,True):need(exact(owner.call(method,dict(params,fbx_profile_diagnostics=flag)),baseline),'Diagnostic switch changed accepted model/result identity')
                record['cases'].append(dict(encoding=encoding,label=case['label'],source=identity(bad),repair=identity(good),expected=expected))
        # Attribution follows the actual second donor, not its take name.
        donor_folder=source_root/'donor';donor_folder.mkdir()
        (donor_folder/'color.png').write_bytes(png(2,2,[c for y in range(2)for x in range(2)for c in(x*255,y*255,(x^y)*255,255)]))
        bad=donor_folder/'bad.fbx';good=donor_folder/'good.fbx';base=donor_folder/'base.fbx'
        bad.write_text(sources()[5]['bad'],encoding='utf-8',newline='\n');good.write_text(sources()[5]['good'],encoding='utf-8',newline='\n');base.write_text(fixture.scene(skin=True),encoding='utf-8',newline='\n')
        params=dict(source=str(base),animations=[dict(source=str(good),clip=0,name='First'),dict(source=str(bad),clip=0,name='Second')])
        omitted,row=owner.rejected('asset.import',params);retain_legacy('asset.import',params,omitted,row)
        disabled,off=owner.rejected('asset.import',dict(params,fbx_profile_diagnostics=False));need(exact(disabled,omitted)and off['raw_error']==row['raw_error'],'Donor false legacy error changed')
        donor_nodes=owner.call('asset.source.inspect',dict(source=str(good),section='nodes',limit=64))
        donor_matches=[node['index']for node in donor_nodes['items']if node['name']=='Mesh'];need(donor_nodes['next_offset']is None and len(donor_matches)==1,'Actual donor source owner')
        wanted=dict(type='fbx_material_profile',version=1,reason='transparency',feature='opacity_or_transparency',node=donor_matches[0],node_name='Mesh',material_slot=0,material_name='Analytic',observed_count=None,limit=None,source_role='animation_donor',donor_index=1)
        enabled,_=owner.rejected('asset.import',dict(params,fbx_profile_diagnostics=True));need(exact(enabled,dict(code=omitted['code'],message=omitted['message'],data=wanted)),'Complete actual second-donor attribution')
        repaired=copy.deepcopy(params);repaired['animations'][1]['source']=str(good)
        result=owner.call('asset.import',repaired);retain_legacy('asset.import',repaired,result)
        need(result['animations']==2,'Same source repair did not compose both explicitly named takes')
        need(exact(owner.call('asset.import',dict(repaired,fbx_profile_diagnostics=True)),result),'Diagnostic switch changed repaired donor composition')
        # Every new reason/boundary also traverses the composition catch. The
        # second donor is the same original rig/take; its material alone rejects.
        for number,case in enumerate(sources()[:5]):
            folder=source_root/('donor-profile-'+str(number));folder.mkdir()
            (folder/'color.png').write_bytes((donor_folder/'color.png').read_bytes())
            bad=folder/'bad.fbx';good=folder/'good.fbx';base=folder/'base.fbx'
            bad.write_text(case['bad'],encoding='utf-8',newline='\n');good.write_text(case['good'],encoding='utf-8',newline='\n');base.write_text(fixture.scene(skin=True),encoding='utf-8',newline='\n')
            params=dict(source=str(base),animations=[dict(source=str(good),clip=0,name='First'),dict(source=str(bad),clip=0,name='Second')])
            omitted,row=owner.rejected('asset.import',params);retain_legacy('asset.import',params,omitted,row)
            need(set(omitted)=={'code','message'},'Legacy donor-profile error gained fields')
            disabled,off=owner.rejected('asset.import',dict(params,fbx_profile_diagnostics=False));need(exact(disabled,omitted)and off['raw_error']==row['raw_error'],'Profile-donor false changed raw legacy error')
            nodes=owner.call('asset.source.inspect',dict(source=str(good),section='nodes',limit=64));matches=[node['index']for node in nodes['items']if node['name']==case['node']]
            need(nodes['next_offset']is None and len(matches)==1,'Independent repaired donor source owner')
            wanted=dict(type='fbx_material_profile',version=1,reason=case['reason'],feature=case['feature'],node=matches[0],node_name=case['node'],material_slot=case['slot'],material_name=case['material'],observed_count=case['observed'],limit=case['limit'],source_role='animation_donor',donor_index=1)
            enabled,_=owner.rejected('asset.import',dict(params,fbx_profile_diagnostics=True));need(exact(enabled,dict(code=omitted['code'],message=omitted['message'],data=wanted)),'Complete new second-donor profile witness')
            repaired=copy.deepcopy(params);repaired['animations'][1]['source']=str(good)
            result=owner.call('asset.import',repaired);retain_legacy('asset.import',repaired,result);need(result['animations']==2,'Profile repair did not compose both named takes')
            for flag in(False,True):need(exact(owner.call('asset.import',dict(repaired,fbx_profile_diagnostics=flag)),result),'Profile switch changed accepted donor composition')
        # Invalid flag types reject before missing source I/O and preserve state.
        for method in('asset.import','asset.source.inspect'):
            for value in(None,0,1,'true',[],{}):
                error,_=owner.rejected(method,dict(source=str(source_root/'absent.fbx'),fbx_profile_diagnostics=value),-32602)
                need(error==dict(code=-32602,message='fbx_profile_diagnostics must be Boolean.'),'Exact invalid-kind rejection')
        # Opt-in never relabels existing155 dependency or unrelated skin errors.
        boundaries=[fixture.textured(color='missing.png'),fixture.scene(skin=True,dq=True)]
        for index,text in enumerate(boundaries):
            source=source_root/('boundary-'+str(index)+'.fbx');source.write_text(text,encoding='utf-8',newline='\n')
            for method in('asset.import','asset.source.inspect'):
                params=dict(source=str(source));ordinary,row=owner.rejected(method,params);retain_legacy(method,params,ordinary,row)
                for flag in(False,True):
                    result,switched=owner.rejected(method,dict(params,fbx_profile_diagnostics=flag));need(exact(result,ordinary)and switched['raw_error']==row['raw_error'],'Opt-in changed an unrelated old error')
        owner.close();candidate_world=owner.world;record['candidate_packages']=package_tree(candidate_world);owner=None
        if args.legacy_binary:
            old_record={};record['owners'].append(old_record);owner=Owner(args.legacy_binary.resolve(),args.output/'legacy',deadline,old_record)
            for expected in legacy_requests:
                if expected['raw_error']is None:actual=owner.call(expected['method'],expected['params']);raw=None
                else:actual,row=owner.rejected(expected['method'],expected['params'],expected['result']['code']);raw=row['raw_error']
                need(exact(actual,expected['result'])and raw==expected['raw_error'],'Omitted request changed complete old-native result/error bytes')
                record['legacy_pairs'].append(dict(method=expected['method'],params=expected['params'],error=raw is not None,exact=True))
            owner.close();legacy_world=owner.world;owner=None
            record['legacy_packages']=package_tree(legacy_world)
            need(exact(record['candidate_packages'],record['legacy_packages']),'Complete old/current cooked package inventories differ')
            record['package_byte_pairs']=[]
            for name,info in record['candidate_packages'].items():
                current=Path(str(candidate_world)+'.assets')/name;previous=Path(str(legacy_world)+'.assets')/name
                need(current.read_bytes()==previous.read_bytes(),'Complete old/current cooked package bytes differ')
                record['package_byte_pairs'].append(dict(path=name,identity=info,exact=True))
        record['passed']=True
    except BaseException:record['errors'].append(traceback.format_exc())
    finally:
        if owner is not None:
            try:owner.close()
            except BaseException:record['errors'].append(traceback.format_exc());record['passed']=False
        record['seconds']=time.monotonic()-started
        record['source_pins']={str(path):identity(path)for path in(Path(__file__),Path(fixture.__file__),ROOT/'tests/texture_fixture.py',ROOT/'tests/switchback_game_service.py')}
        # All native owners/streams are closed before census. Reserve a full
        # 8 MiB and one file for publication under the fixed 128 MiB/4096 cap.
        try:record['output_before_evidence']=census(args.output)
        except BaseException:record['passed']=False;record['errors'].append(traceback.format_exc())
        raw=(json.dumps(record,indent=2,allow_nan=False)+'\n').encode();need(len(raw)<=8*1024**2,'Bounded final report')
        with(args.output/'evidence.json').open('xb')as stream:stream.write(raw)
    print(json.dumps(dict(passed=record['passed'],cases=len(record['cases']),legacy_pairs=len(record['legacy_pairs']),errors=record['errors'])),flush=True)
    return 0 if record['passed']else 1


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--output',type=Path,default=ROOT/'build/fbx-material-profile'/uuid.uuid4().hex);parser.add_argument('--legacy-binary',type=Path)
    raise SystemExit(run(parser.parse_args()))
