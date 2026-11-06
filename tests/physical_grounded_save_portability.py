#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Grounded-save restore and continuation test over intact consumer artifacts.

Requires an external process-tree supervisor. --source-report names the original
consumer evidence.json; its complete parent directory may be copied from another
OS without editing the recorded world, assets, slots or report. The test launches
bounded native CLI owners, retains literal replies and complete archives, and
requires natural closure. It builds nothing and opens no renderer or window.
A rejected load remains a failed test. This is not an engine service or benchmark.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import stat
import sys
import time
import traceback
import uuid
from types import SimpleNamespace

sys.dont_write_bytecode = True
SELF = Path(__file__).resolve()
ROOT = SELF.parents[1]
sys.path.insert(0, str(ROOT/'tests'))
import world_physical_contract as f

MIB = 1024**2
SLOTS = ('planted-stationary', 'planted-active', 'takeover-first', 'takeover-future', 'passive-next')


def recorded_parts(name):
    """Parse a recorded absolute path without applying host filesystem rules."""
    f.need(type(name) is str and name and '\x00' not in name, 'Literal recorded path required')
    text = name.replace('\\', '/')
    if text.startswith('//'):
        prefix, tail = '//', text[2:].split('/')
        f.need(len(tail) >= 2, 'Complete recorded network root required')
    elif text.startswith('/'):
        prefix, tail = '/', text[1:].split('/')
    else:
        drive, separator, rest = text.partition('/')
        f.need(separator == '/' and len(drive) == 2 and drive[0] in
               'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz' and drive[1] == ':',
               'Absolute recorded POSIX or Windows path required')
        prefix, tail = drive.upper(), rest.split('/')
    f.need(all(part not in ('', '.', '..') and ':' not in part for part in tail),
           'Nonescaping recorded path required')
    return (prefix, *tail)


def source_mapper(source, source_report):
    """Map only the original consumer namespace to the supplied report's parent."""
    world = recorded_parts(source['artifacts']['world'])
    f.need(len(world) >= 4 and world[-2:] == ('primary', 'world.json'),
           'Original world must be consumer/primary/world.json')
    original_root, actual_root = world[:-2], source_report.parent.resolve()

    def local(name):
        parts = recorded_parts(name)
        f.need(len(parts) > len(original_root) and parts[:len(original_root)] == original_root,
               'Recorded path is outside the original consumer directory: '+name)
        mapped = actual_root.joinpath(*parts[len(original_root):])
        f.need(mapped.is_relative_to(actual_root), 'Mapped path escaped the supplied consumer directory')
        return mapped

    return local


def identity(path):
    before = path.stat(follow_symlinks=False)
    f.need(stat.S_ISREG(before.st_mode) and not path.is_symlink() and
           not getattr(before, 'st_file_attributes', 0) & 0x400, 'Ordinary input: '+str(path))
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    after = path.stat(follow_symlinks=False)
    f.need((before.st_size, before.st_mtime_ns, before.st_ino) ==
           (after.st_size, after.st_mtime_ns, after.st_ino), 'Input changed while hashing')
    return dict(bytes=after.st_size, sha256=digest)


def read(path):
    f.need(path.stat().st_size <= 16*MIB, 'Bounded source record')
    def pairs(items):
        out = {}
        for key, value in items:
            f.need(key not in out, 'Duplicate original JSON key')
            out[key] = value
        return out
    return json.loads(path.read_bytes(), object_pairs_hook=pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(AssertionError('Nonfinite JSON: '+value)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, required=True)
    parser.add_argument('--source-report', type=Path, required=True,
                        help='Original consumer evidence.json beside its complete copied primary/ directory')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.cli, args.source_report, args.output = (p.resolve() for p in (args.cli, args.source_report, args.output))
    f.need(not args.output.exists(), 'Fresh output required')
    source = read(args.source_report)
    slots = [r['slot'] for r in source['archives']]
    f.need(len(slots)==len(set(slots))==16, 'All16 distinct original slot rows required')
    f.need(source['format'] == 'poima.physical-character-grounded-handoff.v1' and source['passed'] is True and
           source['errors'] == [] and len(source['owners']) == 3 and
           all(o['natural_exit'] is True and o['forced_cleanup'] is False and o['exit_code'] == 0
               and o['reader_failure'] is None for o in source['owners']), 'Original successful closed consumer required')
    local = source_mapper(source, args.source_report)
    source_world, source_saves = local(source['artifacts']['world']), local(source['artifacts']['saves_root'])
    source_assets = Path(str(source_world)+'.assets')
    source_package = local(source['artifacts']['asset_package'])
    f.need(source_world.is_relative_to(args.source_report.parent) and
           source_saves == source_world.parent/'saves' and
           identity(source_world) == source['authored_identity']['world'], 'Exact source world and save ownership')
    f.need(source_package.parent == source_assets and
           identity(source_package) == source['authored_identity']['package'], 'Exact original cooked package')
    pins = {str(p): identity(p) for p in (SELF, args.cli, args.source_report)}
    for module in tuple(sys.modules.values()):
        name = getattr(module, '__file__', None)
        if name and Path(name).suffix == '.py' and Path(name).resolve().is_relative_to(ROOT):
            helper = Path(name).resolve(); pins[str(helper)] = identity(helper)
    copies = [(source_world, args.output/'world.json')]
    asset_files = list(source_assets.iterdir())
    f.need(0 < len(asset_files) <= 16, 'Bounded original cooked asset directory')
    copies += [(p, Path(str(args.output/'world.json')+'.assets')/p.name) for p in asset_files]
    selected = []
    for slot in slots:
        matches = [r for r in source['archives'] if r['slot'] == slot]
        f.need(len(matches) == 1, 'Unique original selected archive')
        row = matches[0]; original, manifest = local(row['path']), local(row['manifest_path'])
        f.need(original.parent == source_saves/('slot-'+slot) and manifest == original.parent/'current.json' and
               identity(original) == dict(bytes=row['bytes'], sha256=row['sha256']) and
               identity(manifest) == row['manifest_identity'], 'Original whole archive and manifest identities')
        current = read(manifest)['payload']['current']
        f.need(current['file'] == original.name and current['generation'] == 1 and
               {k: current[k] for k in ('bytes', 'sha256')} == identity(original), 'Literal slot generation/payload join')
        members = list(original.parent.iterdir())
        f.need(0 < len(members) <= 8, 'Bounded original slot membership')
        copies += [(p, args.output/'saves'/('slot-'+slot)/p.name) for p in members]
        document=read(original);rigs=document['snapshot']['payload']['physical']['rigs']
        f.need(len(rigs)==1 and rigs[0]['membership']['mode'] in ('animated','passive'), 'One original grounded owner')
        selected.append(dict(slot=slot, tick=row['tick'], source_archive=str(original), identity=identity(original),
            mode=rigs[0]['membership']['mode'], rig=rigs[0]['entity']))
    for original, _ in copies:
        pins[str(original)] = identity(original)
    f.need(sum(pins[str(p)]['bytes'] for p, _ in copies) <= 32*MIB, 'Bounded byte-preserved copies')
    args.output.mkdir(parents=True)
    report = dict(format='poima.physical-grounded-save-portability.v1', passed=False, errors=[], calls=[], owners=[],
                  archives=[], cases=[], cold_cases=[], whole_pairs=[], inputs=pins, source_report=str(args.source_report), copies=[],
                  scope='All16 original slots; complete source equality, rejected-command atomicity, destination warm continuation and one fresh owner per five distinct original states. No cross-OS future trajectory equality claim.')
    f.ARGS = SimpleNamespace(binary=args.cli, windows_interop=False)
    f.REPORT, f.DEADLINE, f.MAX_CALLS, f.MAX_RECORDED = report, time.monotonic()+360, 1000, 16*MIB
    try:
        for original, target in copies:
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open('xb') as stream:
                stream.write(original.read_bytes())
            f.need(identity(target) == pins[str(original)], 'Copied bytes differ from original')
            report['copies'].append(dict(source=str(original), path=str(target), identity=identity(target)))
        unique={}
        for row in selected:unique.setdefault((row['identity']['bytes'],row['identity']['sha256']),row)
        f.need(len(unique)==5, 'Original cohort has exactly five complete distinct saved states')
        sequence=0
        def snapshot(session,tick,session_id,label):
            nonlocal sequence
            sequence+=1;slot='probe-'+str(sequence)
            f.archive(session,args.output/'saves',tick,slot,session_id)
            row=dict(report['archives'][-1],label=label)
            return row
        def pair(label,left,right):
            a,b=Path(left),Path(right);ar,br=a.read_bytes(),b.read_bytes()
            byte_equal=ar==br;typed_equal=f.exact(read(a),read(b))
            report['whole_pairs'].append(dict(label=label,left=str(a),right=str(b),
                complete_byte_equal=byte_equal,complete_typed_equal=typed_equal,left_identity=identity(a),right_identity=identity(b)))
            f.need(byte_equal and typed_equal,'Complete byte/typed archive difference: '+label)
        def load(session,chosen,current,record):
            state=game=None
            if current['session'] is not None:
                state=session.rpc('runtime.inspect',dict(session_id=current['session']))
                game=session.rpc('runtime.gameplay.inspect',dict(session_id=current['session'],tick=state['tick']))
            fresh=uuid.uuid4().hex
            try:
                value=session.rpc('save.load',dict(request_id=uuid.uuid4().hex,configuration_generation=1,
                    slot=chosen['slot'],expected_generation=1,revision=1,expected_session_id=current['session'],
                    expected_tick=state['tick'] if state else None,expected_gameplay_revision=game['revision'] if game else None,
                    expected_structure_revision=state['structure_revision'] if state else None,
                    expected_material_revision=state['material_revision'] if state else None,
                    expected_ui_revision=state['ui_revision'] if state else None,
                    expected_control_sequence=state['control_sequence'] if state else None,new_session_id=fresh))
            finally:
                if report['calls'] and report['calls'][-1]['request']['method']=='save.load':
                    record.setdefault('literal_loads',[]).append(report['calls'][-1])
            f.need(value.get('session_id',value.get('new_session_id'))==fresh and value['tick']==chosen['tick'],'Actual restored session/tick')
            current.update(session=fresh,tick=chosen['tick']);record['restore_accepted']=True
        def advance(session,chosen,current):
            commands=[(1,dict(physical_rigs=[dict(entity=chosen['rig'],activate=True)])),(4,{})] if chosen['mode']=='animated' else [(6,{})]
            for ticks,extra in commands:
                old=current['tick'];value=session.rpc('runtime.step',dict(session_id=current['session'],request_id=uuid.uuid4().hex,
                    expected_tick=old,ticks=ticks,**extra))
                f.need(type(value['tick'])is int and value['tick']==old+ticks,'Completed literal continuation tick')
                current['tick']=value['tick']
        def configure(session):
            value=session.rpc('save.configure',dict(request_id=uuid.uuid4().hex,expected_generation=0,root=str(args.output/'saves')))
            f.need(value['generation']==1,'Owned copied save configuration')
        def bounded():
            size=0;members=0
            for base,dirs,names in os.walk(args.output,followlinks=False):
                for name in dirs+names:
                    path=Path(base)/name;info=path.stat(follow_symlinks=False);members+=1
                    f.need(not path.is_symlink() and not getattr(info,'st_file_attributes',0)&0x400,'Ordinary owned output')
                    if stat.S_ISREG(info.st_mode):size+=info.st_size
            f.need(members<=4096 and size<=112*MIB,'Bounded copied worlds/logs/archives with report reserve')
        primary=None
        try:
            primary=f.Session(args.output/'owner-primary',args.output/'world.json');configure(primary)
            current=dict(session=None,tick=None)
            for chosen in selected:
                case=dict(chosen,restore_accepted=False,passed=False,errors=[]);report['cases'].append(case)
                try:
                    load(primary,chosen,current,case)
                    base=snapshot(primary,current['tick'],current['session'],'source-'+chosen['slot'])
                    pair('source-'+chosen['slot'],chosen['source_archive'],base['path']);case['initial']=base
                    # Existing ownership conflicts must still reject atomically.
                    extra=dict(grounding=[dict(entity=chosen['rig'],slot=1,enabled=True,weight=1.)])
                    if chosen['mode']=='animated':extra['physical_rigs']=[dict(entity=chosen['rig'],activate=True)]
                    primary.rpc('runtime.step',dict(session_id=current['session'],request_id=uuid.uuid4().hex,
                        expected_tick=current['tick'],ticks=1,**extra),error=-32040)
                    rejected=snapshot(primary,current['tick'],current['session'],'rejected-'+chosen['slot'])
                    pair('atomic-rejection-'+chosen['slot'],base['path'],rejected['path'])
                    advance(primary,chosen,current)
                    future=snapshot(primary,current['tick'],current['session'],'future-'+chosen['slot']);case['continuation']=future
                    load(primary,chosen,current,case)
                    warm=snapshot(primary,current['tick'],current['session'],'warm-origin-'+chosen['slot'])
                    pair('warm-origin-'+chosen['slot'],base['path'],warm['path'])
                    advance(primary,chosen,current)
                    replay=snapshot(primary,current['tick'],current['session'],'warm-future-'+chosen['slot'])
                    pair('warm-future-'+chosen['slot'],future['path'],replay['path'])
                    bounded();case['passed']=True
                except Exception:case['errors'].append(traceback.format_exc())
        except Exception:report['errors'].append(traceback.format_exc())
        finally:
            if primary is not None:
                try:primary.close()
                except Exception:report['errors'].append(traceback.format_exc())
                f.need(primary.process.poll() is not None,'Prior native owner still live; no duplicate producer')
        for index,chosen in enumerate(unique.values()):
            case=dict(chosen,restore_accepted=False,passed=False,errors=[]);report['cold_cases'].append(case);session=None
            try:
                f.need(time.monotonic()<f.DEADLINE,'Finite cold-owner admission deadline')
                session=f.Session(args.output/('owner-cold-'+str(index)),args.output/'world.json');configure(session)
                current=dict(session=None,tick=None);load(session,chosen,current,case)
                cold=snapshot(session,current['tick'],current['session'],'cold-origin-'+chosen['slot'])
                pair('cold-origin-'+chosen['slot'],chosen['source_archive'],cold['path']);case['initial']=cold
                advance(session,chosen,current)
                future=snapshot(session,current['tick'],current['session'],'cold-future-'+chosen['slot']);case['continuation']=future
                prior=next(r for r in report['cases']if r['slot']==chosen['slot'])
                f.need('continuation'in prior,'Primary destination continuation unavailable; no substitute oracle')
                pair('cold-future-'+chosen['slot'],prior['continuation']['path'],future['path'])
                bounded();case['passed']=True
            except Exception:case['errors'].append(traceback.format_exc())
            finally:
                if session is not None:
                    try:session.close()
                    except Exception:case['passed']=False;case['errors'].append(traceback.format_exc())
                    f.need(session.process.poll() is not None,'Prior cold owner still live; no duplicate producer')
        f.need(all(identity(Path(name)) == value for name, value in pins.items()), 'Frozen original inputs changed')
        f.need(all(identity(Path(row['path']))==row['identity'] for row in report['copies']), 'Copied source world/assets/slots changed')
        f.need(identity(args.output/'world.json') == source['authored_identity']['world'], 'Copied authored world changed')
        report['passed'] = (not report['errors'] and len(report['cases'])==16 and len(report['cold_cases'])==5 and
            all(c['passed'] for c in report['cases']+report['cold_cases']) and len(report['owners'])==6 and
            len(report['archives'])==90 and len(report['whole_pairs'])==74 and
            all(o['natural_exit']is True and o['forced_cleanup']is False and o['exit_code']==0 and o['reader_failure']is None for o in report['owners']))
    except Exception:
        report['errors'].append(traceback.format_exc())
    with (args.output/'report.json').open('x') as stream:
        json.dump(report, stream, indent=2, allow_nan=False); stream.write('\n')
    print(json.dumps(dict(passed=report['passed'], report=str(args.output/'report.json'),
                         results=[dict(slot=c['slot'], restore_accepted=c['restore_accepted'], passed=c['passed'])
                                  for c in report['cases']])))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
