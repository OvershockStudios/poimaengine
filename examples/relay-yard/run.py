#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Author and launch Relay Yard through the native world service.

Caller supplies the original licensed Kenney sources and a prebuilt gameplay
artifact. No downloads, compilation, image/audio/mesh generation or source edits.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import traceback
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('poima_locomotion_yard_reference',
                                            ROOT/'examples/locomotion-yard/run.py')
locomotion = importlib.util.module_from_spec(spec)
spec.loader.exec_module(locomotion)
uid = locomotion.uid
TYPE = 'Poima.Examples.RelayYardGame'
ACTIONS = {'begin': 911, 'welcome.load': 913, 'welcome.refresh': 915, 'menu': 901, 'close': 930,
           'save': 931, 'load': 932, 'refresh': 933, 'retry': 934,
           'fov.minus': 940, 'fov.plus': 941, 'ui.minus': 942, 'ui.plus': 943,
           'sensitivity.minus': 944, 'sensitivity.plus': 945,
           'gain.minus': 946, 'gain.plus': 947, 'preferences.reset': 948}
CELLS = ((1101, 'Ruby power cell', (-6, 1.2, 5), (.8, .04, .08)),
         (1102, 'Emerald power cell', (-2, 1.2, 5.4), (.03, .6, .15)),
         (1103, 'Amber power cell', (4.5, 1.2, 5), (.9, .55, .05)))
AUDIO_CADENCE = 'c0870000000000000000000000000001'


def audio_manifest(path):
    path=Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError('Supply a regular Relay Yard audio manifest.')
    with path.open('rb') as stream:
        payload=stream.read(65537)
    if len(payload)>65536:raise ValueError('Relay Yard audio manifest exceeds 64 KiB.')
    def unique(pairs):
        result={}
        for key,value in pairs:
            if key in result:raise ValueError('Duplicate audio manifest key: '+key)
            result[key]=value
        return result
    def nonfinite(value):
        raise ValueError('Nonfinite audio manifest number: '+value)
    return json.loads(payload.decode('utf-8'),object_pairs_hook=unique,parse_constant=nonfinite)


def relay_fixture():
    ops = []
    for identifier, name, position, color in CELLS:
        ops.append(dict(op='template.set', id=uid(identifier), name=name, components={
            'Transform': dict(position=list(position), rotation=[0,0,0,1], scale=[.4,.4,.4]),
            'MeshRenderer': dict(primitive='box', visible=True, albedo=list(color)),
            'BoxCollider': dict(half_extents=[.5,.5,.5], motion='static', mass=1,
                                friction=.5, restitution=0)}))
    # The terminal is beside the delivery station, outside the courier capsule
    # goal. A native ray must hit this collider; UI has no completion action.
    ops += [dict(op='entity.create', id=uid(600), name='Relay terminal', parent=None),
            dict(op='component.set', id=uid(600), type='Transform', value=dict(
                position=[5.2,1.2,-.8], rotation=[0,0,0,1], scale=[.6,1.4,.6])),
            dict(op='component.set', id=uid(600), type='MeshRenderer', value=dict(
                primitive='box', visible=True, albedo=[.64,.25,.32])),
            dict(op='component.set', id=uid(600), type='BoxCollider', value=dict(
                half_extents=[.5,.5,.5], motion='static', mass=1, friction=.5, restitution=0))]
    # Existing labels retain stable identities used by the reviewed helpers.
    # Remove old action buttons/containers in the same atomic transaction that
    # reparents their labels; there is no intermediate invalid hierarchy.
    for identifier in (690,691,*locomotion.ACTIONS.values()):
        ops.append(dict(op='ui.element.remove', id=uid(identifier)))
    dp = lambda value: dict(unit='dp',value=value)
    percent = lambda value: dict(unit='percent',value=value)

    def ui(identity, parent, kind, name, text='', action=None, visible=True, **properties):
        ops.append(dict(op='ui.element.set',id=uid(identity),element=dict(
            parent=uid(parent) if parent is not None else None,name=name,kind=kind,text=text,
            action=action,visible=visible,enabled=True,**properties)))

    def button(identity,parent,name,action,order=0,brand=False):
        ui(identity,parent,'button',name,name,action,layout=dict(
            width=percent(100),height=dp(32),shrink=0,order=order,padding=[0,8,0,8]),
            style=dict(font_size=14,color='#f7e8eb',background_color='#722f37' if brand else '#30252a',
                border_color='#4f1a23',border_width=1,border_radius=4,text_align='center',
                hover=dict(background_color='#4f1a23'),focus=dict(border_color='#d46a7e')))

    ui(900,None,'panel','Relay HUD',layout=dict(position='absolute',left=dp(16),bottom=dp(16),
        width=percent(75),height=dp(88),direction='column',gap=4,padding=[8,12,8,12],
        hit_test='pass_through'),style=dict(background_color='#191519dd',border_width=0))
    for order,(identifier,text) in enumerate(((700,'Begin your shift, or load a checkpoint.'),
                                             (701,'Courier waiting for three power cells.'),
                                             (702,'Save progress from the menu.'))):
        ui(identifier,900,'label',text,text,layout=dict(width=percent(100),height=dp(20),
            shrink=0,order=order),style=dict(font_size=14,color='#f7e8eb'))
    ui(699,None,'label','Relay Yard title','Relay Yard',layout=dict(position='absolute',
        left=dp(16),top=dp(12),width=dp(250),height=dp(30)),
        style=dict(font_size=22,color='#d46a7e'))
    ui(901,None,'button','Open menu after Tab','Menu [Tab releases cursor]','menu',
        layout=dict(position='absolute',right=dp(16),top=dp(12),width=dp(224),height=dp(34)),
        style=dict(font_size=14,color='#f7e8eb',background_color='#722f37',border_width=0,
                   hover=dict(background_color='#4f1a23'),focus=dict(border_color='#d46a7e')))
    # Responsive retained layout; overflow stays scrollable at large UI scales.
    for identifier,title,visible in ((910,'Welcome',True),(920,'Checkpoint and preferences',False)):
        ui(identifier,None,'panel',title,visible=visible,
            layout=dict(position='absolute',left=percent(3),top=percent(8),width=dp(420),
                max_width=percent(90),height=percent(78),direction='column',gap=6,
                padding=[16,16,16,16],overflow='auto',hit_test='capture'),
            style=dict(background_color='#191519f5',border_color='#4f1a23',border_width=1,
                       border_radius=0,color='#f7e8eb',font_size=14))
    ui(912,910,'label','Welcome instructions',
        'Recover three power cells. The courier carries them around the cover. '
        'Use the relay after arrival.\nWASD move | mouse look | E use within 3 m | '
        'Tab releases the cursor for Menu.\nBegin starts the shift; Load restores saved progress.',
        layout=dict(width=percent(100),height=dp(138),shrink=0,order=0),
        style=dict(font_size=16,color='#f7e8eb'))
    button(911,910,'Begin shift','begin',1,True)
    button(913,910,'Load checkpoint','load',2)
    ui(914,910,'label','Welcome checkpoint status','Save progress or load your last relay checkpoint.',
        layout=dict(width=percent(100),height=dp(60),shrink=0,order=3),
        style=dict(font_size=14,color='#f7e8eb'))
    button(915,910,'Refresh checkpoint status','refresh',4)
    ui(922,920,'label','Menu checkpoint status','Save progress or load your last relay checkpoint.',
        layout=dict(width=percent(100),height=dp(60),shrink=0,order=0),
        style=dict(font_size=14,color='#f7e8eb'))
    ui(921,920,'label','Current preferences','Refresh to inspect native player preferences.',
        layout=dict(width=percent(100),height=dp(100),shrink=0,order=0),
        style=dict(font_size=14,color='#f7e8eb'))
    menu_actions = (('close','Close and resume'),('save','Save checkpoint'),('load','Load checkpoint'),
        ('refresh','Refresh status'),('retry','Retry courier route'),('fov.minus','FOV −'),
        ('fov.plus','FOV +'),('ui.minus','UI scale −'),('ui.plus','UI scale +'),
        ('sensitivity.minus','Pointer sensitivity −'),('sensitivity.plus','Pointer sensitivity +'),
        ('gain.minus','Master gain −'),('gain.plus','Master gain +'),('preferences.reset','Reset preferences'))
    for order,(action,name) in enumerate(menu_actions,1):
        button(ACTIONS[action],920,name,action,order,action=='close')
    # Retained text crosshair is existing UI styling, not generated image data.
    ui(950,None,'label','Center reticle','+',layout=dict(position='absolute',left=percent(50),
        top=percent(50),width=dp(16),height=dp(20)),style=dict(color='#f7e8eb',font_size=18))
    return ops


class OwnedRelay(locomotion.OwnedGame):
    def author(self, source_directory):
        authored=super().author(source_directory)
        self.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=authored['revision'],
                                      ops=relay_fixture()))
        # Include the added terminal in native nav baking. Source import,
        # retargeting, original clips and licensing provenance remain unchanged.
        navigation=self.rpc('world.navigation.bake',dict(revision=4,
            profile=dict(**authored['capsule'],climb=0)))['asset']
        self.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=4,
                                      ops=[dict(op='navigation.set',asset=navigation)]))
        authored.update(revision=5,navigation=navigation,relay_cells=CELLS,terminal=uid(600),
                        module_type=TYPE,actions=ACTIONS)
        if getattr(self.args,'audio_directory',None):
            self.author_audio(self.args.audio_directory)
        return authored

    def author_audio(self,directory):
        directory=Path(directory).resolve(strict=True)
        # The complete published manifest pins original licensed inputs, native
        # conversion and each supplied WAV. No decoder or downloader runs here.
        expected=audio_manifest(ROOT/'examples/relay-yard/audio/manifest.json')
        actual=audio_manifest(directory/'manifest.json')
        if json.dumps(actual,sort_keys=True,separators=(',',':'))!= \
                json.dumps(expected,sort_keys=True,separators=(',',':')):
            raise ValueError('Supply the published Relay Yard audio manifest and unchanged files.')
        for row in list(actual['packs'].values())+list(actual['sounds'].values()):
            for path_key,hash_key,size_key in (('license_path','license_sha256','license_bytes'),
                ('path','sha256','bytes'),('original_path','original_sha256','original_bytes')):
                if path_key not in row:continue
                relative=Path(row[path_key]);path=directory/relative
                if relative.is_absolute() or '..' in relative.parts or path.is_symlink() or \
                        not path.is_file() or path.stat().st_size!=row[size_key] or \
                        hashlib.sha256(path.read_bytes()).hexdigest()!=row[hash_key]:
                    raise ValueError('Audio input differs: '+row[path_key])
        schemas=[s for s in self.manifest['schemas'] if s['id']==AUDIO_CADENCE]
        if len(schemas)!=1:raise ValueError('Compile the Relay Yard audio cadence component first.')
        fields={f['name']:f for f in schemas[0]['fields']}
        assets,ops={},[]
        for name,row in actual['sounds'].items():
            clip=self.rpc('asset.audio.import',dict(source=self.native(directory/row['path'])))
            if clip['frames']!=row['frames'] or clip['channels']!=1 or clip['sample_rate']!=48000:
                raise ValueError('Native imported audio profile differs.')
            asset=clip['asset'];assets[name]=asset;pack=actual['packs'][row['pack']]
            provenance=self.rpc('asset.provenance.create',dict(record=dict(
                format='poima.asset-provenance',version=1,asset=asset,kind='audio',
                title='Relay Yard — '+name,creator='Kenney',source=pack['source'],
                license=dict(identifier='CC0-1.0',notice=(directory/pack['license_path']).read_text()+
                    '\nOriginal '+row['original_archive_member']+'; deterministic mono 48kHz PCM16 conversion '
                    'relay-audio-convert-v1. Recipe manifest SHA-256 '+hashlib.sha256((directory/'manifest.json').read_bytes()).hexdigest()+'.'),
                inputs=[dict(sha256=row['original_sha256'],bytes=row['original_bytes']),
                        dict(sha256=pack['license_sha256'],bytes=pack['license_bytes'])])))
            ops.append(dict(op='asset.provenance.set',asset=asset,records=[provenance['record']]))
        for identifier,parent,position,sound,gain in (
            (610,101,[0,-.2,-.25],'pickup',.45),(611,101,[0,-.2,-.25],'denied',.35),
            (612,300,[0,1.4,0],'relay',.65),(613,600,[0,.7,0],'relay',.65),
            (620,100,[0,.15,0],'step-a',.6),(621,100,[0,.15,0],'step-b',.6),
            (622,300,[0,.15,0],'step-a',.8),(623,300,[0,.15,0],'step-b',.8)):
            ops += [dict(op='entity.create',id=uid(identifier),parent=uid(parent),name='Relay sound '+str(identifier)),
                dict(op='component.set',id=uid(identifier),type='Transform',value=dict(
                    position=position,rotation=[0,0,0,1],scale=[1,1,1])),
                dict(op='component.set',id=uid(identifier),type='AudioEmitter',value=dict(
                    asset=assets[sound],gain=gain,loop=False,enabled=True))]
        for actor,a,b,stride in ((100,620,621,1.6),(300,622,623,1.1)):
            values={f['id']:f['default'] for f in fields.values()}
            for key,value in dict(Enabled=1,StepEmitterA=uid(a),StepEmitterB=uid(b),
                                  StrideMeters=stride,Gain=.45).items():
                values[fields[key]['id']]=value
            ops.append(dict(op='component.set',id=uid(actor),type='game:'+AUDIO_CADENCE,value=values))
        revision=self.rpc('world.inspect')['revision']
        self.rpc('world.transact',dict(request_id=uuid.uuid4().hex,base_revision=revision,ops=ops))
        self.authored.update(revision=revision+1,audio=dict(assets=assets,cadence_type=AUDIO_CADENCE,
            manifest_sha256=hashlib.sha256((directory/'manifest.json').read_bytes()).hexdigest()))

    def start(self):
        if self.session:self.rpc('runtime.stop',dict(session_id=self.session))
        self.session,self.tick,self.revision=uuid.uuid4().hex,0,0
        self.rpc('runtime.start',dict(session_id=self.session,
                                     revision=self.rpc('world.inspect')['revision']))

    def step(self, count=1, error=None):
        state = self.inspect()
        result = self.rpc('runtime.step', dict(request_id=uuid.uuid4().hex,
            session_id=self.session, expected_tick=self.tick,
            expected_structure_revision=state['structure_revision'], ticks=count), error)
        if error is None:
            self.session, self.tick = result['current_session_id'], result['current_tick']
        return result

    def config(self):
        result=super().config()
        if not self.args.descriptor:result['type']=TYPE
        return result

    def control(self,action):
        state,module=self.inspect(),self.module()
        result=self.rpc('runtime.ui.activate',dict(request_id=uuid.uuid4().hex,session_id=self.session,
            expected_tick=self.tick,expected_ui_revision=state['ui_revision'],
            expected_control_sequence=state['control_sequence'],expected_gameplay_revision=module['revision'],
            expected_structure_revision=state['structure_revision'],id=uid(ACTIONS[action])))
        self.session,self.tick=result['current_session_id'],result['current_tick']
        return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','source-directory','manifest','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    for name in ('hostfxr','bridge','assembly','descriptor'):
        parser.add_argument('--'+name,type=Path)
    parser.add_argument('--windows-interop',action='store_true')
    parser.add_argument('--gpu',type=int,default=0)
    parser.add_argument('--author-only',action='store_true')
    parser.add_argument('--audio-directory',type=Path,
        help='Opt-in published licensed sound set (examples/relay-yard/audio); enables native player audio.')
    args=parser.parse_args()
    if bool(args.descriptor)==bool(args.assembly):parser.error('Supply assembly or descriptor, exclusively.')
    if not args.descriptor and not(args.hostfxr and args.bridge):parser.error('CoreCLR needs hostfxr and bridge.')
    if not args.binary.is_file() or not args.manifest.is_file():parser.error('Supply existing binary and generated component manifest.')
    if args.output.exists():parser.error('Use a new output directory; failures retain their artifacts.')
    args.output.mkdir(parents=True)

    def native(path):
        value=str(Path(path).resolve())
        return subprocess.check_output(['wslpath','-w',value],text=True,timeout=10).strip() \
            if args.windows_interop and os.name!='nt' else value

    record=dict(passed=False,calls=[],owners=[],scope='Interactive authoring launcher; qualification recorded separately')
    game=None
    completed=False
    try:
        game=OwnedRelay(args,args.output/'world.json','relay-interactive',record,time.monotonic()+86400,
            native,json.loads(args.manifest.read_text(encoding='utf-8')))
        game.author(args.source_directory)
        (args.output/'authored.json').write_text(json.dumps(game.authored,indent=2)+'\n',encoding='utf-8')
        if not args.author_only:
            game.start();game.load();game.saves(args.output/'saves')
            print('Begin shift, collect three cells with E, follow the courier, use the relay. '
                  'Tab releases the cursor; Menu pauses. Close resumes; click the scene to capture look.',flush=True)
            terminal=game.rpc('runtime.play',dict(request_id=uuid.uuid4().hex,session_id=game.session,
                expected_tick=0,controller=uid(100),camera=uid(101),mode='interactive',audio=bool(args.audio_directory),
                width=1280,height=720,gpu=args.gpu,samples=1,frames_in_flight=1),timeout=86400)
            record['player']=terminal
            if not terminal['success'] or terminal['nvrhi_errors']:
                raise RuntimeError('Native player reported a failed render lifetime.')
        completed=True
    except BaseException:
        record['error']=traceback.format_exc()
        raise
    finally:
        try:
            if game is not None:game.close()
        except BaseException:
            record['cleanup_error']=traceback.format_exc()
            raise
        finally:
            record['passed']=completed and 'cleanup_error' not in record
            (args.output/'commands.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
