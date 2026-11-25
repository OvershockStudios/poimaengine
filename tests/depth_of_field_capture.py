#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify authored depth of field, focused detail, and relocated Vulkan output."""

import argparse
import json
import math
from pathlib import Path
import tempfile
import time

from height_fog_capture import environment
from scene_capture import pixels
from retained_capture import CAMERA, Fixture, component, expect_error, file_hash, new_id, transform, tree_hashes, uid

SIZE = (192, 128)
LENS = dict(vertical_fov=60, near=.10, far=60)
PROFILE = dict(focus_distance_m=4, focus_band_m=.08, radius_at_infinity_px=15, max_radius_px=18)
FOREGROUND = uid(640)
DEPTHS = (1.65, 4, 8.2)
STRIPES = tuple((8 + 64 * index, 56 + 64 * index, depth) for index, depth in enumerate(DEPTHS))
RING = ((.404, 0), (.202, .350), (-.202, .350), (-.404, 0), (-.202, -.350), (.202, -.350),
        (.765, .249), (.473, .650), (0, .804), (-.473, .650), (-.765, .249), (-.765, -.249),
        (-.473, -.650), (0, -.804), (.473, -.650), (.765, -.249))


def samples(index):
    return [dict(x=x, y=SIZE[1]//2) for x in range(index*64, (index+1)*64)]


def render_reference(line, x):
    def radius(point):
        z = line[point]['depth']
        # Perspective inverse projection using metres, independent of the shader's inverse-depth form.
        meters = math.inf if z == 1 else LENS['near'] / (1 - z * (1 - LENS['near']/LENS['far']))
        delta = (meters - PROFILE['focus_distance_m']) / meters if math.isfinite(meters) else 1
        band = PROFILE['focus_band_m'] / meters
        return min(PROFILE['max_radius_px'], PROFILE['radius_at_infinity_px'] * max(0, abs(delta)-band)) * (-1 if delta < 0 else 1), meters

    color = line[x]['raw_hdr']
    amount, meters = radius(x)
    if abs(amount) > .5:
        rows = [color]
        for px, py in RING:
            dx, dy = round(px*abs(amount)), round(py*abs(amount))
            neighbor = x + dx
            tap, tap_meters = radius(neighbor)
            sufficiently_distant = tap_meters >= meters / 1.0125 if math.isfinite(meters) else not math.isfinite(tap_meters)
            if tap * amount > 0 and sufficiently_distant and math.hypot(dx, dy) <= abs(tap) + .75:
                rows.append(line[neighbor]['raw_hdr'])
        color = [sum(channels) / len(rows) for channels in zip(*rows, strict=True)]
    # Samples used in the numerical comparison have one uniform plane spanning their search radius.
    assert all(line[k]['depth'] == line[x]['depth'] for k in range(x-15, x+16))
    return color


class FocusFixture(Fixture):
    def author(self, client):
        rev = client.call('world.inspect')['revision']
        operations = [dict(op='component.remove', id=uid(1), type='MeshRenderer'),
                      dict(op='component.remove', id=uid(5), type='Light'),
                      component(uid(4),'LightingEnvironment',environment()),
                      dict(op='entity.create', id=CAMERA, name='Focus bands camera'),
                      component(CAMERA,'Transform',transform((0,0,0))),
                      component(CAMERA,'Camera',LENS | dict(viewmodel=dict(vertical_fov=65,near=.015,far=10))),
                      dict(op='entity.create',id=FOREGROUND,name='Separate foreground',parent=CAMERA),
                      component(FOREGROUND,'Transform',transform([0,-.12,-.32]) | dict(scale=[.085,.055,.015])),
                      component(FOREGROUND,'MeshRenderer',dict(primitive='box',albedo=[0,0,0],visible=True,
                        cast_shadows=False,receive_shadows=False)),
                      component(FOREGROUND,'PbrMaterial',dict(base_color=[0,0,0],roughness=.7,metallic=0,
                        emissive=[0,.81,.0],double_sided=False)),
                      component(FOREGROUND,'CameraDraw',dict(camera=CAMERA,mode='viewmodel'))]
        for group, (left, _, depth) in enumerate(STRIPES):
            half_height = math.tan(math.radians(LENS['vertical_fov']*.5))*depth
            pixel_width = 2 * half_height / SIZE[1]
            for band in range(12):
                key=uid(500+12*group+band)
                point = [(left+band*4+2-SIZE[0]/2)*pixel_width, 0, -depth-.008]
                xform=transform(point) | dict(scale=[4*pixel_width,.98*half_height,.016])
                # Use unequal levels so misplaced foreground terms are visible in every color channel.
                energy=[.86,.58,.30] if band%2==0 else [.055,.082,.125]
                operations.extend((dict(op='entity.create', id=key, name=f'Plane {group} stripe {band}'),
                    component(key,'Transform',xform),
                    component(key,'MeshRenderer',dict(primitive='box',albedo=[0,0,0],visible=True,
                                                        cast_shadows=False,receive_shadows=False)),
                    component(key,'PbrMaterial',dict(base_color=[0,0,0],roughness=.7,metallic=0,
                                                    emissive=energy,double_sided=False))))
        result=client.call('world.transact',dict(request_id=new_id(),base_revision=rev,ops=operations))
        assert result['committed']
        return result['revision']

    def select(self, client, rev, profile):
        ops=[component(uid(4),'LightingEnvironment',environment() | dict(depth_of_field=profile))]
        reply=client.call('world.transact',dict(base_revision=rev,request_id=new_id(),ops=ops))
        assert reply['committed']
        return reply['revision']

    def compare(self, first, active, group):
        before=first['report']['render_diagnostics']['scene_products']['probes']
        after=active['report']['render_diagnostics']['scene_products']['probes']
        assert len(before)==len(after)==64
        line={row['x']:row for row in before}
        start=STRIPES[group][0]+19
        depths=[]
        deltas=[]
        error=[]
        for row in after:
            x=row['x']
            assert row['raw_hdr']==line[x]['raw_hdr'] and row['depth']==line[x]['depth']
            if start<=x<start+10:
                depths.append(row['depth'])
                expected=render_reference(line,x)
                error.append(max(abs(a-b) for a,b in zip(expected,row['resolved_hdr'],strict=True)))
                deltas.append(math.dist(row['resolved_hdr'],row['raw_hdr']))
        assert len(error)==10 and max(error)<.0012,(group,error)
        dz=LENS['near']/(1-depths[0]*(1-LENS['near']/LENS['far']))
        assert abs(dz-DEPTHS[group])<.003
        if group==1:
            assert max(deltas)==0,deltas
        else:
            assert min(deltas)>.09,deltas
        return dict(group=group,near_or_far=group!=1,probes=len(error),max_hdr_error=max(error),
                    max_change=max(deltas),camera_depth_m=dz)

    def draw(self, client, session, label, **kwargs):
        target = self.root / (label+'.bmp')
        params = self.parameters(session, 0, target, width=SIZE[0], height=SIZE[1], **kwargs)
        started = time.perf_counter()
        report = client.call('runtime.capture', params, timeout=60)
        elapsed = time.perf_counter() - started
        assert report['hardware'] and report['capture_written'] and report['nvrhi_errors']==0
        assert report['tick']==0 and report['session_id']==session and report['camera']==CAMERA
        assert report['frames_presented']==params['capture_frames']
        assert (report['width'],report['height'],report['samples'])==(*SIZE,params['samples'])
        row = dict(image=file_hash(target),time_s=elapsed,report=report)
        self.data['captures'][label] = row
        return row

    def run(self):
        self.cli('project','create',self.native(self.project),'--name','Depth of Field Fixture')
        baselines=[]
        with self.owner('authoring') as client:
            self.revision=self.author(client)
            frozen=self.revision
            assert not client.call('world.dependencies').get('needs_depth_of_field')
            for invalid in (PROFILE | dict(max_radius_px=25), PROFILE | dict(focus_distance_m=True),
                            PROFILE | dict(focus_band_m=-1), {k:v for k,v in PROFILE.items() if k!='max_radius_px'}):
                params=dict(base_revision=frozen,request_id=new_id(),ops=[component(uid(4),'LightingEnvironment',
                    environment() | dict(depth_of_field=invalid))])
                expect_error(-32602,'world.transact',client,params)
                assert client.call('world.inspect')['revision']==frozen
            if self.args.gpu is not None:
                session=self.mount(client)
                for i in range(3):
                    baselines.append(self.draw(client,session,f'baseline-{i}',scene_product_probes=samples(i)))
                base_depth=self.draw(client,session,'baseline-depth',scene_debug_view='depth')
                if self.args.msaa:
                    base_msaa=self.draw(client,session,'baseline-msaa',samples=4)
                client.call('runtime.stop',dict(session_id=session))
            self.revision=self.select(client,frozen,PROFILE | dict(max_radius_px=0))
            assert client.call('world.dependencies')['needs_depth_of_field']
            if self.args.gpu is not None:
                session=self.mount(client)
                zero=self.draw(client,session,'zero',scene_product_probes=samples(0))
                assert zero['image']==baselines[0]['image']
                assert zero['report']['render_diagnostics']['depth_of_field']['reason']=='zero_radius'
                client.call('runtime.stop',dict(session_id=session))
            self.revision=self.select(client,self.revision,PROFILE)
            session=self.mount(client)
            world=client.call('runtime.lighting',dict(session_id=session,tick=0))['depth_of_field']
            assert abs(world['focus_band_m']-PROFILE['focus_band_m'])<1e-7
            assert all(world[k]==PROFILE[k] for k in PROFILE if k!='focus_band_m')
            self.data['published_lighting']=world
            if self.args.gpu is not None:
                profiles=[]
                for i,baseline in enumerate(baselines):
                    active=self.draw(client,session,f'active-{i}',scene_product_probes=samples(i))
                    self.active_image=active['image']
                    assert active['image']!=baseline['image']
                    assert active['report']['render_diagnostics']['depth_of_field']==dict(
                        active=True,reason='active',buffer_bytes=SIZE[0]*SIZE[1]*8)
                    profiles.append(self.compare(baseline,active,i))
                self.data['numerical_checks']=profiles
                before=pixels(self.root/'baseline-0.bmp')
                after=pixels(self.root/'active-0.bmp')
                foreground=[(x,y) for y,row in enumerate(before) for x,rgb in enumerate(row)
                            if rgb[0]==rgb[2]==0 and rgb[1]>130]
                assert len(foreground)>180
                assert all(before[y][x]==after[y][x] for x,y in foreground)
                self.data['identical_foreground_pixels']=len(foreground)
                depth=self.draw(client,session,'diagnostic',scene_debug_view='depth')
                assert depth['image']==base_depth['image']
                assert depth['report']['render_diagnostics']['depth_of_field']['reason']=='diagnostic'
                if self.args.msaa:
                    msaa=self.draw(client,session,'multisample',samples=4)
                    assert msaa['image']==base_msaa['image']
                    assert msaa['report']['render_diagnostics']['depth_of_field']['reason']=='multisample'
                if self.args.fsr:
                    fsr=self.draw(client,session,'composed-fsr',reconstruction='fsr3_native')
                    diag=fsr['report']['render_diagnostics']
                    assert diag['depth_of_field']['active'] and diag['reconstruction']['active']
                    assert diag['reconstruction']['composition_mask_bytes']==SIZE[0]*SIZE[1]
        if self.args.runtime_root:
            self.relocated_focus()

    def relocated_focus(self):
        export=self.root/'Built'
        self.cli('project','build',self.native(self.manifest),'--output',self.native(export),
                 '--runtime',self.native(self.args.runtime_root))
        bundle=self.root/'Relocated'
        export.rename(bundle)
        saves=self.root/'External saves'
        saves.mkdir()
        self.project.rename(self.root/'Hidden source')
        game=bundle/'game.json'
        if self.args.windows_interop:
            # Windows can execute the exported PE without a POSIX mode bit.
            # WSL's process launcher requires that bit on an ext4 export.
            binary=bundle/json.loads(game.read_bytes())['engine']['executable']
            binary.chmod(binary.stat().st_mode | 0o100)
        before=tree_hashes(bundle)
        with self.owner('relocated',game=game,saves=saves) as client:
            live=client.call('runtime.status')
            assert live['active'] and live['tick']==0
            captured=self.draw(client,live['session_id'],'relocated',scene_product_probes=samples(2))
            assert captured['image']==self.active_image
            assert captured['report']['render_diagnostics']['depth_of_field']['active']
        assert before==tree_hashes(bundle) and not self.project.exists()
        self.data['immutable_relocated_export_files']=len(before)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--gpu',type=int)
    parser.add_argument('--lighting-path',choices=('forward','deferred'),default='forward')
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--runtime-root',type=Path)
    parser.add_argument('--msaa',action='store_true')
    parser.add_argument('--fsr',action='store_true')
    args=parser.parse_args()
    if not __debug__ or (args.msaa and args.lighting_path!='forward') or (
            args.gpu is None and (args.msaa or args.fsr or args.runtime_root)):
        parser.error('Select a GPU for rendering, or omit render options for authoring checks.')
    args.binary=args.binary.resolve()
    args.output.mkdir(parents=True,exist_ok=True)
    root=Path(tempfile.mkdtemp(dir=args.output,prefix='depth-of-field-'))
    result=dict(passed=False,cli=[],owners=[],captures={},gpu=args.gpu,path=args.lighting_path,binary_sha256=file_hash(args.binary))
    try:
        FocusFixture(args,root,result).run()
        result['passed']=True
    finally:
        path=root/'evidence.json'
        path.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
        print(path,flush=True)


if __name__=='__main__':
    main()
