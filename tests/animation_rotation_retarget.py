#!/usr/bin/env python3
"""Explicit rotation-retarget protocol checks with original analytic FBX oracles.

The optional --source-directory is a caller-supplied, unmodified Kenney Animated
Characters Protagonists directory containing Model/characterMedium.fbx and
Animations/{idle,run,jump}.fbx. No download, build, GPU or animation fabrication
is performed. That optional oracle uses native normalized ORIGINAL source poses
and original target skin data, independently applying quaternion-chain/FK math;
it is not an independent FBX parser, contact solver or loop-quality assessment.
"""
# SPDX-License-Identifier: Apache-2.0
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import time
import unittest
import uuid

from rotation_retarget_fixture import expected_motion, write_fixtures
from frame_transfer_fixture import column_major

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/python'))
from poima_client import WorldClient, RpcError

ARGS = None
DEADLINE = 0
POLICY = 'reference-rotation-v1'
MATRIX_ERROR, VERTEX_ERROR = 3e-5, 5e-5
RECORD = dict(passed=False, calls=[], owners=[], source_files={}, checks=[], source_replacements=[])


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def timeout(maximum=60):
    remaining = DEADLINE-time.monotonic()
    if remaining <= 0: raise TimeoutError('Qualification deadline exhausted')
    return min(maximum, remaining)
def native(path):
    value = str(Path(path).resolve())
    if ARGS.windows_interop:
        return subprocess.check_output(['wslpath', '-w', value], text=True, timeout=timeout(10)).strip()
    return value

def unit(q):
    length = math.sqrt(sum(x*x for x in q))
    if not math.isfinite(length) or length < 1e-12: raise AssertionError('Invalid original orientation')
    return [x/length for x in q]
def conjugate(q): return [-q[0], -q[1], -q[2], q[3]]
def product(a, b):
    x,y,z,w=a; X,Y,Z,W=b
    return [w*X+x*W+y*Z-z*Y, w*Y-x*Z+y*W+z*X, w*Z+x*Y-y*X+z*W, w*W-x*X-y*Y-z*Z]
def matrix(position, rotation, scale):
    x,y,z,w=unit(rotation)
    return [[(1-2*y*y-2*z*z)*scale[0], (2*x*y-2*z*w)*scale[1], (2*x*z+2*y*w)*scale[2], position[0]],
            [(2*x*y+2*z*w)*scale[0], (1-2*x*x-2*z*z)*scale[1], (2*y*z-2*x*w)*scale[2], position[1]],
            [(2*x*z-2*y*w)*scale[0], (2*y*z+2*x*w)*scale[1], (1-2*x*x-2*y*y)*scale[2], position[2]],
            [0,0,0,1]]
def multiply(a, b): return [[sum(a[i][k]*b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]
def point(m, p): return [sum(m[i][k]*p[k] for k in range(3))+m[i][3] for i in range(3)]
def from_columns(m): return [[m[c*4+r] for c in range(4)] for r in range(4)]
def inverse(m):
    rows=[list(row)+[float(i==j) for j in range(4)] for i,row in enumerate(m)]
    for c in range(4):
        p=max(range(c,4),key=lambda r:abs(rows[r][c]))
        if abs(rows[p][c])<1e-12: raise AssertionError('Singular original target skin transform')
        rows[c],rows[p]=rows[p],rows[c]; divisor=rows[c][c]; rows[c]=[x/divisor for x in rows[c]]
        for r in range(4):
            if r!=c:
                coefficient=rows[r][c];rows[r]=[x-coefficient*y for x,y in zip(rows[r],rows[c])]
    return [row[4:] for row in rows]
def rotated(q, p): return product(product(unit(q), [*p,0]), conjugate(unit(q)))[:3]
def paths(rows):
    indexed={row['index']:row for row in rows}; result={}
    def visit(i):
        if i not in result:
            parent=indexed[i]['parent'];result[i]=(visit(parent) if parent>=0 else ())+ (indexed[i]['name'],)
        return result[i]
    for i in indexed: visit(i)
    if len(set(result.values()))!=len(result): raise AssertionError('Ambiguous original hierarchy paths')
    return result

def qchains(rows):
    indexed={row['index']:row for row in rows}; result={}
    def visit(i):
        if i not in result:
            parent=indexed[i]['parent'];q=unit(indexed[i]['rotation']);result[i]=unit(product(visit(parent),q)) if parent>=0 else q
        return result[i]
    for i in indexed: visit(i)
    return result

def worlds(rows):
    indexed={row['index']:row for row in rows}; result={}
    def visit(i):
        if i not in result:
            row=indexed[i];local=matrix(row['position'],row['rotation'],row['scale']);result[i]=multiply(visit(row['parent']),local) if row['parent']>=0 else local
        return result[i]
    for i in indexed: visit(i)
    return result


class RotationRetargetContract(unittest.TestCase):
    def setUp(self):
        self.directory=ARGS.output/self._testMethodName;self.directory.mkdir()
        self.sources=write_fixtures(self.directory/'sources');self.world=self.directory/'world.json';self.client=None
        self.addCleanup(self.close);self.open()
        self.call('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=0,
            ops=[dict(op='entity.create',id='1'*32,name='Retained retarget authoring')]))
        self.baseline=self.imported('skin')

    def open(self): self.client=WorldClient.open(str(ARGS.binary),native(self.world),close_timeout=10)
    def close(self):
        owner,self.client=self.client,None
        if owner is None:return
        row=dict(test=self.id(),exit_code=None,cleanup_error=None);RECORD['owners'].append(row)
        try:owner.close()
        except BaseException as error:row['cleanup_error']=str(error);raise
        finally:row.update(exit_code=owner.transport.returncode,process_id=owner.transport.process_id,
            stderr=owner.transport.stderr_tail,stderr_truncated=owner.transport.stderr_truncated)
        self.assertEqual(row['exit_code'],0);self.assertEqual(row['stderr'],'');self.assertFalse(row['stderr_truncated'])
    def call(self,method,params=None,error=None):
        row=dict(test=self.id(),method=method,params=params or {});RECORD['calls'].append(row)
        try:result=self.client.call(method,row['params'],timeout=timeout())
        except RpcError as failure:
            row['error']=dict(code=failure.code,message=failure.message,data=failure.data)
            self.assertIsNotNone(error,failure.message);self.assertEqual(failure.code,error);return failure
        row['result']=result;self.assertIsNone(error,'Expected rejection');return result
    def page(self,method,params,limit=64):
        rows=[];offset=0
        for _ in range(1024):
            result=self.call(method,dict(params,offset=offset,limit=limit));rows.extend(result['items']);nxt=result.get('next_offset')
            if nxt is None:return rows
            self.assertIsInstance(nxt,int);self.assertGreater(nxt,offset);offset=nxt
        self.fail('Bounded pagination exceeded')
    def source(self,name):
        path=self.sources/(name+'.fbx');RECORD['source_files'][str(path.resolve())]=sha(path);return native(path)
    def inspected(self,name,**options):
        result=self.call('asset.source.inspect',dict(source=self.source(name),**options));self.assertIs(result['published'],False)
        self.assertRegex(result['model_sha256'],r'^[0-9a-f]{64}$');self.assertEqual(result['format'],'poima.source-inspection.v1')
        self.assertLessEqual(len(result['diagnostics']),64);return result
    def source_nodes(self,name,pose=None,limit=64):
        params=dict(source=self.source(name),section='nodes')
        if pose is not None:params['pose']=pose
        return self.page('asset.source.inspect',params,limit)
    def policy(self,base='skin',donor='gauge_rest',source_pose=None,target_pose=None,positions=None,alignment=None):
        result=dict(policy=POLICY,source_pose=source_pose or dict(kind='rest'),target_pose=target_pose or dict(kind='rest'),
            expected_source_model_sha256=self.inspected(donor)['model_sha256'],
            expected_target_model_sha256=self.inspected(base)['model_sha256'],
            positions=positions or dict(kind='target_reference'),scales='target_reference')
        if alignment is not None:result['alignment_rotation']=alignment
        return result
    def imported(self,base,donor=None,policy=None,*,clip=1,name='RetargetedMotion',error=None,frame=None):
        params=dict(source=self.source(base))
        if donor is not None:
            selection=dict(source=self.source(donor),clip=clip,name=name)
            if policy is not None:selection['retarget']=policy
            if frame is not None:selection['frame_transfer']=frame
            params['animations']=[selection]
        return self.call('asset.import',params,error)
    def state(self):
        store=Path(str(self.world)+'.assets')
        return (self.call('world.inspect'),self.call('world.history'),self.world.read_bytes(),
            {p.relative_to(store).as_posix():sha(p) for p in sorted(store.rglob('*')) if p.is_file()})
    def rejected(self,base,donor,policy,*,error=-32050,clip=1,frame=None):
        before=self.state();result=self.imported(base,donor,policy,clip=clip,error=error,frame=frame)
        self.assertEqual(self.state(),before,'Rejected policy changed world/history or populated asset store');return result
    def diagnostics(self,result,policy):
        text='\n'.join(result['diagnostics']);self.assertIn(POLICY,text)
        for key in ('source_model_sha256','target_model_sha256'):
            values=re.findall(re.escape(key)+r'[^0-9a-f]+([0-9a-f]{64})(?![0-9a-f])',text)
            self.assertEqual(values,[policy['expected_'+key]])
        self.assertIn('position_policy='+policy['positions']['kind'],text);self.assertIn('scale_policy=target_reference',text)
        self.assertIn('affine_motion_equivalence=not-guaranteed',text)
        self.assertIn('discarded_translation_channels=',text);self.assertIn('discarded_scale_channels=',text)
        RECORD['checks'].append(dict(kind='explicit_policy_provenance',diagnostics=result['diagnostics']))
    def sample_motion(self,asset,*,name='RetargetedMotion',times=(0,.137,.375,.813,1),**expectations):
        nodes=self.page('asset.inspect',dict(asset=asset,section='nodes'));clips=self.page('asset.inspect',dict(asset=asset,section='animations'))
        clip=next(row for row in clips if row['name']==name);self.assertAlmostEqual(clip['duration'],1,delta=1e-7)
        ids={row['name']:row['index'] for row in nodes};mesh=next(row for row in nodes if row['primitives']);observations=[]
        for t in times:
            wanted=expected_motion(t,**expectations);pose=self.page('asset.animation.sample',dict(asset=asset,clip=clip['index'],time=t,loop=False,section='nodes'));indexed={row['index']:row for row in pose}
            for label in ('Root','Child'):
                error=max(abs(x-y) for x,y in zip(indexed[ids[label]]['world'],wanted[label.lower()]));self.assertLessEqual(error,MATRIX_ERROR,f'{label} original FK at {t}')
                RECORD['checks'].append(dict(kind='analytic_target_global_matrix',node=label,time=t,maximum_error=error))
            verts=self.page('asset.animation.sample',dict(asset=asset,clip=clip['index'],time=t,loop=False,section='vertices',node=mesh['index'],primitive=mesh['primitives'][0]))
            self.assertEqual([v['index'] for v in verts],list(range(3)))
            error=max(abs(x-y) for v,p in zip(verts,wanted['vertices']) for x,y in zip(v['world_position'],p));self.assertLessEqual(error,VERTEX_ERROR)
            RECORD['checks'].append(dict(kind='analytic_original_target_inverse_bind_skin',time=t,maximum_error=error));observations.append(dict(time=t,nodes=pose,vertices=verts))
        return observations

    def test_target_reference_preserves_proportions_scales_and_discarded_take_duration(self):
        before=self.state()
        for donor in ('gauge_rest','gauge_proportion','gauge_scale'):
            with self.subTest(donor=donor):
                policy=self.policy(donor=donor);result=self.imported('skin',donor,policy)
                self.diagnostics(result,policy);self.sample_motion(result['asset'])
                self.assertEqual(self.state()[:3],before[:3])
        policy=self.policy(base='target_scale',donor='gauge_scale');result=self.imported('target_scale','gauge_scale',policy)
        self.sample_motion(result['asset'],child_scale=(1,1.25,1))
        policy=self.policy(donor='move_donor');dropped=self.imported('skin','move_donor',policy,clip=0,name='DiscardedTranslation')
        self.sample_motion(dropped['asset'],name='DiscardedTranslation',stationary=True)
        self.assertIn('discarded_translation_channels=1','\n'.join(dropped['diagnostics']))

    def test_selected_original_child_reference_delta_and_alignment(self):
        reference=dict(kind='sample',clip=0,time=.5);nodes=self.source_nodes('gauge_sample');child=next(n['index'] for n in nodes if n['name']=='Child')
        position=dict(kind='reference_delta',nodes=[child],scale=1.5)
        policy=self.policy(donor='gauge_sample',source_pose=reference,positions=position);result=self.imported('skin','gauge_sample',policy)
        self.diagnostics(result,policy);self.sample_motion(result['asset'],delta_scale=1.5)
        # Original aligned root position is deliberately different. Alignment
        # transfers orientation; target-reference root position remains (2,0,0).
        aligned=self.policy(donor='gauge_aligned',alignment=[0,0,math.sqrt(.5),math.sqrt(.5)])
        result=self.imported('skin','gauge_aligned',aligned);self.sample_motion(result['asset'])
        # Rest reference for the proportion variant is Child X=1.1, whereas
        # its original Motion begins X=1: the explicit delta must retain -.1.
        child=next(n['index'] for n in self.source_nodes('gauge_proportion') if n['name']=='Child')
        proportion=self.policy(donor='gauge_proportion',positions=dict(kind='reference_delta',nodes=[child],scale=1))
        result=self.imported('skin','gauge_proportion',proportion);self.sample_motion(result['asset'],delta_scale=1,child_y=.9)

    def test_sampled_reference_before_filter_preserves_target_sample_and_hash_identity(self):
        policy=self.policy(base='base_with_reference',donor='gauge_sample',source_pose=dict(kind='sample',clip=0,time=.5),target_pose=dict(kind='sample',clip=0,time=.5))
        result=self.imported('base_with_reference','gauge_sample',policy,name='SelectedOriginalMotion');self.assertEqual(result['animations'],3)
        self.diagnostics(result,policy);self.sample_motion(result['asset'],name='SelectedOriginalMotion',child_x=.5)
        self.assertEqual(self.imported('base_with_reference','gauge_sample',policy,name='SelectedOriginalMotion')['asset'],result['asset'])
        names=[c['name'] for c in self.page('asset.inspect',dict(asset=result['asset'],section='animations'))]
        self.assertEqual(names,['Move','Turn','SelectedOriginalMotion'])
        # Explicit retargeting does not silently replace the exact default.
        before=self.state();failure=self.imported('skin','gauge_proportion',error=-32050)
        self.assertEqual(failure.data['policy'],'exact-skeleton-v1');self.assertEqual(self.state(),before)

    def test_wire_hash_reference_rejection_atomicity_and_same_owner_recovery(self):
        valid=self.policy();invalid=[False,{},dict(valid,policy='unknown'),dict(valid,extra=1),dict(valid,scales='source'),
            dict(valid,source_pose=dict(kind='rest',time=0)),dict(valid,source_pose=dict(kind='sample',clip=True,time=0)),
            dict(valid,source_pose=dict(kind='sample',clip=0,time=True)),dict(valid,source_pose=dict(kind='sample',clip=0,time=-1)),
            dict(valid,source_pose=dict(kind='sample',clip=0,time=3601)),dict(valid,target_pose=dict(kind='other')),
            dict(valid,positions={}),dict(valid,positions=dict(kind='target_reference',scale=1)),
            dict(valid,positions=dict(kind='reference_delta',nodes=[],scale=1)),
            dict(valid,positions=dict(kind='reference_delta',nodes=[1,1],scale=1)),
            dict(valid,positions=dict(kind='reference_delta',nodes=[True],scale=1)),
            dict(valid,positions=dict(kind='reference_delta',nodes=[-1],scale=1)),
            dict(valid,positions=dict(kind='reference_delta',nodes=[10000],scale=1)),
            dict(valid,positions=dict(kind='reference_delta',nodes=list(range(65)),scale=1)),
            dict(valid,positions=dict(kind='reference_delta',nodes=[1],scale=0)),
            dict(valid,positions=dict(kind='reference_delta',nodes=[1],scale=100.001)),
            dict(valid,positions=dict(kind='reference_delta',nodes=[1],scale=True)),
            dict(valid,positions=dict(kind='reference_delta',nodes=[1],scale=1,extra=1)),
            dict(valid,alignment_rotation=[0,0,0,0]),dict(valid,alignment_rotation=[0,0,0,1,0]),
            dict(valid,alignment_rotation=[False,0,0,1])]
        for key in ('source','target'):
            field='expected_'+key+'_model_sha256';missing=dict(valid);del missing[field];invalid.append(missing)
            invalid.extend(dict(valid,**{field:value}) for value in ('abc','A'*64,'g'*64,False))
        for value in invalid:
            with self.subTest(policy=value):self.rejected('skin','gauge_rest',value,error=-32602)
        self.rejected('skin','gauge_rest',valid,error=-32602,frame=dict(policy='reference-frame-v1',source_pose=dict(kind='rest'),target_pose=dict(kind='rest')))
        for bad in (dict(valid,expected_source_model_sha256='f'*64),dict(valid,expected_target_model_sha256='f'*64),
                    dict(valid,source_pose=dict(kind='sample',clip=255,time=0)),dict(valid,source_pose=dict(kind='sample',clip=0,time=1.01)),
                    dict(valid,target_pose=dict(kind='sample',clip=0,time=0)),dict(valid,positions=dict(kind='reference_delta',nodes=[9999],scale=1))):
            self.rejected('skin','gauge_rest',bad)
        # Replace ONLY an owned analytic clone after inspection. A normalized
        # model change must invalidate the guard even if the new root position
        # would be discarded by this retarget policy.
        changed=self.sources/'changed_source.fbx';changed.write_bytes((self.sources/'gauge_rest.fbx').read_bytes())
        stale=self.policy(donor='changed_source');previous=sha(changed)
        text=changed.read_text();needle='"A",200,0,0';self.assertIn(needle,text)
        changed.write_text(text.replace(needle,'"A",201,0,0',1),encoding='utf-8',newline='\n')
        RECORD['source_replacements'].append(dict(path=str(changed.resolve()),previous_sha256=previous,replaced_sha256=sha(changed),owned_analytic_only=True))
        self.rejected('skin','changed_source',stale)
        recovered=self.policy(donor='changed_source');self.assertNotEqual(recovered['expected_source_model_sha256'],stale['expected_source_model_sha256'])
        self.sample_motion(self.imported('skin','changed_source',recovered)['asset'],times=(.375,))
        scoped=self.call('world.describe',dict(view='method',name='asset.import'))
        selections=scoped['methods']['asset.import']['properties']['animations']['items']['anyOf'];selection=next(s for s in selections if s.get('type')=='object');schema=selection['properties']['retarget']
        self.assertIs(schema['additionalProperties'],False);self.assertEqual(schema['properties']['policy']['const'],POLICY)
        self.assertEqual(set(schema['required']),{'policy','source_pose','target_pose','expected_source_model_sha256','expected_target_model_sha256','positions','scales'})
        self.sample_motion(self.imported('skin','gauge_rest',valid)['asset'],times=(.375,))

    def test_source_inspection_is_guarded_paginated_unpublished_and_cook_reopens_without_sources(self):
        before=self.state();summary=self.inspected('gauge_sample');self.assertEqual(summary['primitives'],0)
        single=self.source_nodes('gauge_sample',limit=1);bulk=self.source_nodes('gauge_sample');self.assertEqual(single,bulk)
        self.assertEqual([n['index'] for n in bulk],list(range(summary['nodes'])))
        reference=dict(kind='sample',clip=0,time=.5)
        posed=self.source_nodes('gauge_sample',pose=reference);self.assertTrue(all('world' in n for n in posed))
        verified=self.inspected('gauge_sample',expected_model_sha256=summary['model_sha256']);self.assertEqual(verified['model_sha256'],summary['model_sha256'])
        self.call('asset.source.inspect',dict(source=self.source('gauge_sample'),expected_model_sha256='f'*64),error=-32050)
        for source in (False,'','a'*4097,'\0'):
            self.call('asset.source.inspect',dict(source=source),error=-32602)
        for params in (dict(pose=reference),dict(section='animations',pose=reference),dict(section='nodes',pose=dict(kind='sample',clip=True,time=0)),
                       dict(section='other'),dict(limit=0),dict(limit=65),dict(offset=True),dict(extra=1),dict(expected_model_sha256='A'*64)):
            self.call('asset.source.inspect',dict(source=self.source('gauge_sample'),**params),error=-32602)
        for pose in (dict(kind='sample',clip=255,time=0),dict(kind='sample',clip=0,time=1.1)):
            self.call('asset.source.inspect',dict(source=self.source('gauge_sample'),section='nodes',pose=pose),error=-32050)
        self.assertEqual(self.state(),before,'Inspection published content or mutated authoring/history')
        policy=self.policy(donor='gauge_sample',source_pose=reference);result=self.imported('skin','gauge_sample',policy)
        original=self.sample_motion(result['asset']);committed=self.state();self.close()
        retained=self.directory/'retained-owned-sources';self.sources.rename(retained)
        try:
            self.open();self.assertEqual(self.sample_motion(result['asset']),original);reopened=self.state()
            self.assertEqual((reopened[0],reopened[2],reopened[3]),(committed[0],committed[2],committed[3]))
            self.assertTrue(reopened[1]['session_local']);self.assertEqual(reopened[1]['undo_count'],0);self.assertEqual(reopened[1]['redo_count'],0)
        finally:retained.rename(self.sources)

    def test_original_kenney_separate_clips_preserve_target_reference_and_rotation_motion(self):
        if ARGS.source_directory is None:self.skipTest('Caller original Kenney source directory not supplied')
        originals=dict(base=ARGS.source_directory/'Model/characterMedium.fbx',
                       **{name:ARGS.source_directory/'Animations'/(name+'.fbx') for name in ('idle','run','jump')})
        for path in originals.values():
            self.assertTrue(path.is_file());RECORD['source_files'][str(path.resolve())]=sha(path)
        before=self.state()
        infos={};original_nodes={};original_clips={}
        for name,path in originals.items():
            infos[name]=self.call('asset.source.inspect',dict(source=native(path)));self.assertIs(infos[name]['published'],False)
            original_nodes[name]=self.page('asset.source.inspect',dict(source=native(path),section='nodes'))
            original_clips[name]=self.page('asset.source.inspect',dict(source=native(path),section='animations'))
        self.assertEqual(self.state(),before,'Original source inspection published content')
        baseline=self.call('asset.import',dict(source=native(originals['base'])))
        target=original_nodes['base'];target_index={n['index']:n for n in target};targetpaths=paths(target);by_path={p:i for i,p in targetpaths.items()}
        targetchains=qchains(target);targetworlds=worlds(target)
        skins=self.page('asset.inspect',dict(asset=baseline['asset'],section='skins'));skin_joints={}
        required=set()
        for skin in skins:
            records=self.page('asset.animation.skin',dict(asset=baseline['asset'],skin=skin['index']));skin_joints[skin['index']]=records
            for record in records:
                n=record['node']
                while n>=0:required.add(n);n=target_index[n]['parent']
        # Retain immutable original target skin data. Recover bind-space mesh
        # inputs from ORIGINAL target-rest weighted matrices, not retarget output.
        mesh_inputs=[]
        for mesh in (n for n in target if n['primitives']):
            self.assertGreaterEqual(mesh['skin'],0)
            mesh_inverse=inverse(targetworlds[mesh['index']])
            palette=[multiply(multiply(mesh_inverse,targetworlds[j['node']]),from_columns(j['inverse_bind'])) for j in skin_joints[mesh['skin']]]
            for primitive in mesh['primitives']:
                vertices=self.page('asset.animation.sample',dict(asset=baseline['asset'],time=0,section='vertices',node=mesh['index'],primitive=primitive))
                for v in vertices:
                    blended=[[sum(v['weights'][k]*palette[v['joints'][k]][r][c] for k in range(4)) for c in range(4)] for r in range(4)]
                    # Skinning applies the weighted xyz affine map without a
                    # homogeneous divide, even if float weights sum slightly
                    # away from1. Recover in mesh space before world translation.
                    blended[3]=[0,0,0,1]
                    originalpoint=point(inverse(blended),v['position'])
                    mesh_inputs.append(dict(node=mesh['index'],primitive=primitive,index=v['index'],point=originalpoint,joints=v['joints'],weights=v['weights'],uv=v['uv'],skin=mesh['skin']))
        input_by_identity={(v['node'],v['primitive'],v['index']):v for v in mesh_inputs}
        selections=[];profiles=[]
        for name in ('idle','run','jump'):
            clips=original_clips[name];self.assertEqual(len(clips),2)
            self.assertIn('Targeting Pose',clips[0]['name']);self.assertNotIn('Targeting Pose',clips[1]['name']);self.assertGreater(clips[1]['duration'],0)
            nodes=original_nodes[name];hips=[n['index'] for n in nodes if n['name']=='Hips'];self.assertEqual(len(hips),1)
            policy=dict(policy=POLICY,source_pose=dict(kind='sample',clip=clips[0]['index'],time=0),target_pose=dict(kind='rest'),
                expected_source_model_sha256=infos[name]['model_sha256'],expected_target_model_sha256=infos['base']['model_sha256'],
                positions=dict(kind='reference_delta',nodes=hips,scale=1),scales='target_reference')
            selections.append(dict(source=native(originals[name]),clip=clips[1]['index'],name='Original-'+name,retarget=policy))
            profiles.append(dict(name=name,source=originals[name],clip=clips[1],reference_clip=clips[0],hips=hips[0],policy=policy))
        result=self.call('asset.import',dict(source=native(originals['base']),animations=selections))
        self.assertEqual(result['animations'],3);self.assertEqual(result['nodes'],baseline['nodes']);self.assertEqual(result['vertices'],baseline['vertices']);self.assertEqual(result['triangles'],baseline['triangles'])
        cookednodes=self.page('asset.inspect',dict(asset=result['asset'],section='nodes'));self.assertEqual(cookednodes,self.page('asset.inspect',dict(asset=baseline['asset'],section='nodes')))
        cookedskins=self.page('asset.inspect',dict(asset=result['asset'],section='skins'));self.assertEqual(cookedskins,skins)
        for skin in skins:
            self.assertEqual(self.page('asset.animation.skin',dict(asset=result['asset'],skin=skin['index'])),skin_joints[skin['index']])
        inspected_state=self.state();observations=[]
        for target_clip,profile in enumerate(profiles):
            source_nodes=original_nodes[profile['name']];sourcepaths=paths(source_nodes);source_index={n['index']:n for n in source_nodes}
            mapping={s:by_path[p] for s,p in sourcepaths.items() if p in by_path};self.assertTrue(required<=set(mapping.values()))
            refrows=self.page('asset.source.inspect',dict(source=native(profile['source']),section='nodes',pose=dict(kind='sample',clip=profile['reference_clip']['index'],time=0),expected_model_sha256=infos[profile['name']]['model_sha256']))
            refindex={n['index']:n for n in refrows};refchains=qchains(refrows)
            corrections={s:unit(product(conjugate(refchains[s]),targetchains[t])) for s,t in mapping.items()}
            duration=profile['clip']['duration']
            for fraction in (0,.173,.5,.827,1):
                t=duration*fraction
                motion=self.page('asset.source.inspect',dict(source=native(profile['source']),section='nodes',pose=dict(kind='sample',clip=profile['clip']['index'],time=t),expected_model_sha256=infos[profile['name']]['model_sha256']))
                motionindex={n['index']:n for n in motion};chain=qchains(motion);wanted=copy.deepcopy(target);wi={n['index']:n for n in wanted}
                expectedchains={tgt:unit(product(chain[src],corrections[src])) for src,tgt in mapping.items()}
                for src,tgt in mapping.items():
                    parent=source_index[src]['parent'];left=[0,0,0,1] if parent<0 else conjugate(corrections[parent])
                    wi[tgt]['rotation']=unit(product(product(left,motionindex[src]['rotation']),corrections[src]))
                    if src==profile['hips']:
                        delta=[x-y for x,y in zip(motionindex[src]['position'],refindex[src]['position'])];d=rotated(left,delta)
                        wi[tgt]['position']=[x+y for x,y in zip(target_index[tgt]['position'],d)]
                expectedworld=worlds(wanted)
                actual=self.page('asset.animation.sample',dict(asset=result['asset'],clip=target_clip,time=t,loop=False,section='nodes'));ai={n['index']:n for n in actual};actualchains=qchains(actual)
                orientation_error=0;matrix_error=0;position_error=0;scale_error=0
                for tgt in required:
                    src=next(s for s,d in mapping.items() if d==tgt);dot=abs(sum(x*y for x,y in zip(actualchains[tgt],expectedchains[tgt])));orientation_error=max(orientation_error,1-min(1,dot))
                    matrix_error=max(matrix_error,max(abs(x-y) for x,y in zip(ai[tgt]['world'],column_major(expectedworld[tgt]))))
                for tgt in target_index:
                    position_error=max(position_error,max(abs(x-y) for x,y in zip(ai[tgt]['position'],wi[tgt]['position'])))
                    scale_error=max(scale_error,max(abs(x-y) for x,y in zip(ai[tgt]['scale'],target_index[tgt]['scale'])))
                self.assertLessEqual(orientation_error,1e-8);self.assertLessEqual(matrix_error,5e-5);self.assertLessEqual(position_error,3e-6);self.assertLessEqual(scale_error,1e-9)
                actualverts={}
                for mesh in (n for n in target if n['primitives']):
                    for primitive in mesh['primitives']:
                        for v in self.page('asset.animation.sample',dict(asset=result['asset'],clip=target_clip,time=t,loop=False,section='vertices',node=mesh['index'],primitive=primitive)):
                            key=(mesh['index'],primitive,v['index']);original_input=input_by_identity[key]
                            self.assertEqual(v['joints'],original_input['joints']);self.assertEqual(v['weights'],original_input['weights']);self.assertEqual(v['uv'],original_input['uv'])
                            actualverts[key]=v['world_position']
                palettes={(mesh['index'],mesh['skin']):[multiply(multiply(inverse(expectedworld[mesh['index']]),expectedworld[j['node']]),from_columns(j['inverse_bind'])) for j in skin_joints[mesh['skin']]] for mesh in target if mesh['primitives']};vertex_error=0
                for v in mesh_inputs:
                    palette=palettes[(v['node'],v['skin'])];blend=[[sum(v['weights'][k]*palette[v['joints'][k]][r][c] for k in range(4)) for c in range(4)] for r in range(4)]
                    expected=point(expectedworld[v['node']],point(blend,v['point']));observed=actualverts[(v['node'],v['primitive'],v['index'])];vertex_error=max(vertex_error,max(abs(x-y) for x,y in zip(observed,expected)))
                self.assertLessEqual(vertex_error,8e-5)
                row=dict(kind='original_normalized_source_rotation_chain_and_target_skin',clip=profile['name'],time=t,duration=duration,
                    rotation_chain_one_minus_abs_dot=orientation_error,target_global_matrix_max_error=matrix_error,
                    target_local_position_max_error_m=position_error,target_local_scale_max_error=scale_error,weighted_vertex_max_error_m=vertex_error)
                RECORD['checks'].append(row);observations.append(row)
        self.assertEqual(self.state(),inspected_state,'Read-only original motion inspection or cooked sampling changed authoring/assets')
        RECORD['original_source_qualification']=dict(profiles=[dict(name=p['name'],motion=p['clip'],reference=p['reference_clip'],hips=p['hips']) for p in profiles],
            source_models={name:info['model_sha256'] for name,info in infos.items()},target_required_nodes=len(required),weighted_target_vertices=len(mesh_inputs),
            oracle='Native normalized ORIGINAL source/reference local rotations plus independent quaternion-chain/FK/skin math; immutable original target inverse binds and recovered baseline input vertices',
            limits=['No independent FBX parser, GPU, compiled locomotion, contact fit, seamless loop or root-motion extraction claim.'])


def main():
    global ARGS,DEADLINE
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--output',type=Path)
    parser.add_argument('--windows-interop',action='store_true');parser.add_argument('--source-directory',type=Path);parser.add_argument('--timeout',type=float,default=900)
    ARGS=parser.parse_args()
    if not math.isfinite(ARGS.timeout) or not 60<=ARGS.timeout<=3600:parser.error('--timeout must be finite60..3600seconds')
    ARGS.binary=ARGS.binary.resolve(strict=True)
    if ARGS.source_directory is not None:ARGS.source_directory=ARGS.source_directory.resolve(strict=True)
    ARGS.output=(ARGS.output or ROOT/'build/animation-rotation-retarget-contract'/uuid.uuid4().hex).resolve()
    if not ARGS.binary.is_file() or ARGS.output.exists():parser.error('--binary must be a file and --output must be new')
    ARGS.output.mkdir(parents=True);DEADLINE=time.monotonic()+ARGS.timeout
    dependencies=[Path(__file__).with_name(name) for name in ('rotation_retarget_fixture.py','frame_transfer_fixture.py','fbx_fixture.py','texture_fixture.py')]
    RECORD.update(binary_sha256=sha(ARGS.binary),source_sha256=sha(Path(__file__)),dependencies_sha256={p.name:sha(p) for p in dependencies},
        scope='Explicit orientation retarget and target-reference proportions; independent original analytic FK/skin; optional original separate FBX clips with normalized-source oracle. No builds, renderer, loop/contact or compiled-gameplay qualification.')
    try:
        result=unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(RotationRetargetContract))
        RECORD.update(passed=result.wasSuccessful(),tests=result.testsRun,failures=len(result.failures),errors=len(result.errors),skipped=len(result.skipped))
        RECORD['source_hashes_unchanged']=all(Path(p).is_file() and sha(Path(p))==digest for p,digest in RECORD['source_files'].items())
        RECORD['passed'] &= RECORD['source_hashes_unchanged'];RECORD['rpc_count']=len(RECORD['calls']);RECORD['clean_owners']=sum(o['exit_code']==0 and o['cleanup_error'] is None for o in RECORD['owners'])
    finally:(ARGS.output/'evidence.json').write_text(json.dumps(RECORD,indent=2,ensure_ascii=False,allow_nan=False)+'\n',encoding='utf-8')
    return 0 if RECORD['passed'] else 1


if __name__=='__main__':sys.exit(main())
