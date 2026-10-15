#!/usr/bin/env python3
"""Portable soak diagnostics glue; never constructs a Windows Soak or runs main."""
# SPDX-License-Identifier: Apache-2.0
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
sys.path.insert(0,str(ROOT/'tools/performance'))
import memory_phases

SPEC=importlib.util.spec_from_file_location('poima_soak_memory_glue',ROOT/'tests/performance_yard_soak.py')
soak=importlib.util.module_from_spec(SPEC);sys.modules[SPEC.name]=soak;SPEC.loader.exec_module(soak)
CREATION=134000000000000001


class Clock:
    def __init__(self,value=100):self.value=value
    def __call__(self):return self.value


class InlineThread:
    """Run only the injected portable collector, without creating a thread."""
    def __init__(self,target,daemon):self.target=target;self.daemon=daemon;self.joins=[]
    def start(self):self.target()
    def join(self,seconds):self.joins.append(seconds)
    def is_alive(self):return False


def actual_report(*,pid=42,creation=CREATION,offset=0,samples=2):
    """Real sampler with fake provider, independent query clock and no native APIs."""
    ns=iter([110,120,150,210,230,240,250]);now=[0.]
    class Provider:
        creation_filetime=creation
        def __init__(self):self.calls=0;self.closed=0
        def sample(self):
            self.calls+=1
            if self.calls==samples+1:raise KeyboardInterrupt()
            return dict(creation_filetime=self.creation_filetime,working_set_bytes=100+self.calls,
                private_commit_bytes=200+self.calls)
        def close(self):self.closed+=1
    provider=Provider()
    report=soak.memory.collect(soak.memory.Options(pid,duration=1,query_timing=True),
        provider_factory=lambda _:provider,monotonic=lambda:now[0],
        sleep=lambda seconds:now.__setitem__(0,now[0]+seconds),
        monotonic_ns=lambda:next(ns)+offset,clock_origin_ns=100)
    assert provider.closed==1 and report['sample_count']==samples and report['stop_reason']=='interrupted'
    return report


class TimelineContracts(unittest.TestCase):
    def timeline(self):
        clock=Clock();real=memory_phases.PhaseLog
        with patch.object(memory_phases,'PhaseLog',side_effect=lambda origin:real(origin,clock_ns=clock)):
            result=soak.MemoryTimeline(100)
        return result,clock

    def finished_sampler(self,directory,*,name='memory.json',pid=42,creation=CREATION,offset=0,write=True):
        report=actual_report(pid=pid,creation=creation,offset=offset);path=Path(directory)/name
        if write:soak.json_write(path,report)
        return SimpleNamespace(result=report,path=path,thread=SimpleNamespace(is_alive=lambda:False),identity=creation)

    def test_default_helpers_add_no_fields_clocks_phases_or_calls(self):
        game=object.__new__(soak.Soak);record=dict(passed=True,functional_reliability_passed=True,
            harness_rehearsal_passed=False,error='original')
        before=copy.deepcopy(record)
        with (patch.object(soak.time,'monotonic_ns',side_effect=AssertionError('New clock read in legacy path')),
              patch.object(soak,'MemoryTimeline',side_effect=AssertionError('Legacy timeline created'))):
            game.memory_phase('play',cycle=1,observed={'tick':99})
            soak.finalize_memory_timeline(record,None)
        self.assertEqual(record,before)

    def test_sampler_delegates_exact_origin_only_when_enabled(self):
        for origin in (None,100):
            with self.subTest(origin=origin),tempfile.TemporaryDirectory() as directory:
                owner=SimpleNamespace(process=SimpleNamespace(pid=42));captured=[];probes=[]
                class Probe:
                    creation_filetime=CREATION
                    def __init__(self,options):probes.append(options);self.closed=0
                    def close(self):self.closed+=1
                def collect(options,**kwargs):
                    captured.append((options,kwargs))
                    return dict(process=dict(pid=42,creation_filetime_100ns=str(CREATION)),
                        stop_reason='interrupted',samples=[{},{}],version=1 if origin is None else 2)
                with (patch.object(soak.memory,'WindowsProvider',Probe),patch.object(soak.memory,'collect',side_effect=collect),
                      patch.object(soak.threading,'Thread',InlineThread),patch.object(soak.time,'monotonic',return_value=10.),
                      patch.object(soak.time,'monotonic_ns',side_effect=AssertionError('Sampler invents another origin'))):
                    instance=soak.Sampler(owner,Path(directory)/'memory.json',20.,clock_origin_ns=origin)
                    result=instance.finish()
                self.assertEqual(len(captured),1);options,kwargs=captured[0]
                self.assertEqual(options.pid,42);self.assertEqual(options.duration,10.)
                self.assertEqual(options.interval,1);self.assertTrue(options.system_ram)
                self.assertEqual(options.expected_creation_filetime,CREATION)
                self.assertEqual(options.query_timing,origin is not None)
                self.assertEqual(set(kwargs),{'sleep'} if origin is None else {'sleep','clock_origin_ns'})
                if origin is not None:self.assertEqual(kwargs['clock_origin_ns'],100)
                self.assertEqual(result['version'],1 if origin is None else 2)
                self.assertEqual(json.loads(instance.path.read_bytes()),result)
                self.assertEqual(instance.thread.joins,[15]);self.assertEqual(len(probes),1)

    def test_phase_tags_use_only_observed_tick_without_new_rpc(self):
        marks=[];errors=[];game=object.__new__(soak.Soak)
        game.memory_timeline=SimpleNamespace(mark=lambda phase,**tags:marks.append((phase,tags)),
            error=lambda *args:errors.append(args))
        game.host=SimpleNamespace(process=SimpleNamespace(pid=42));game.label='soak'
        game.memory_creation=str(CREATION);game.player_id='1'*32;game.generation=2
        game.sid='2'*32;game.replacements=3;game.memory_cycle=4
        game.rpc=lambda *_:self.fail('Phase metadata added a native RPC')
        game.player=lambda:self.fail('Phase metadata added an owner observation')
        game.memory_phase('save');game.memory_phase('load',cycle=5,
            observed=dict(tick=61,session_id='2'*32,current_tick=41,current_session_id='3'*32))
        self.assertEqual(marks[0],('save',dict(owner='soak',pid=42,creation_filetime_100ns=str(CREATION),
            player_id='1'*32,generation=2,session_id='2'*32,tick=None,runtime_replacements=3,cycle=4)))
        self.assertEqual(marks[1][1]['tick'],41);self.assertEqual(marks[1][1]['session_id'],'3'*32)
        self.assertEqual(marks[1][1]['cycle'],5);self.assertEqual(errors,[])

    def test_partial_fresh_start_cannot_attach_old_identity_to_new_host(self):
        marks=[];errors=[];game=object.__new__(soak.Soak)
        game.memory_timeline=SimpleNamespace(mark=lambda phase,**tags:marks.append((phase,tags)),
            error=lambda *args:errors.append(args))
        game.host=SimpleNamespace(process=SimpleNamespace(pid=99));game.label='fresh'
        game.memory_creation=None;game.player_id='1'*32;game.generation=7
        game.sid='2'*32;game.replacements=3;game.memory_cycle=None
        for phase in ('startup','cleanup'):game.memory_phase(phase)
        for phase,tags in marks:
            self.assertIn(phase,('startup','cleanup'));self.assertEqual(tags['pid'],99)
            for key in ('creation_filetime_100ns','player_id','generation','session_id','tick','runtime_replacements'):
                self.assertIsNone(tags[key],key+' must not come from the prior owner')
        game.memory_phase('startup',observed=dict(player_id='3'*32,generation=1,
            session_id='4'*32,tick=1))
        actual=marks[-1][1];self.assertEqual(actual['player_id'],'3'*32)
        self.assertEqual((actual['generation'],actual['session_id'],actual['tick']),(1,'4'*32,1))
        self.assertIsNone(actual['creation_filetime_100ns']);self.assertIsNone(actual['runtime_replacements'])
        game.memory_creation=str(CREATION+1);game.player_id='3'*32;game.generation=1
        game.sid='4'*32;game.replacements=0;game.memory_phase('startup',observed={'tick':1})
        current=marks[-1][1]
        self.assertEqual((current['pid'],current['creation_filetime_100ns'],current['player_id'],
            current['generation'],current['session_id'],current['tick'],current['runtime_replacements']),
            (99,str(CREATION+1),'3'*32,1,'4'*32,1,0))
        self.assertEqual(errors,[])

    def test_actual_two_owner_phase_and_query_associations_after_cleanup(self):
        timeline,clock=self.timeline()
        with tempfile.TemporaryDirectory() as directory:
            first=self.finished_sampler(directory,name='soak.json')
            second=self.finished_sampler(directory,name='fresh.json',pid=43,creation=CREATION+1,offset=400)
            timeline.attach('soak',first);timeline.attach('fresh',second)
            timeline.mark('play',owner='soak',pid=42,creation_filetime_100ns=str(CREATION))
            clock.value=200;timeline.mark('snapshot',owner='soak',pid=42,creation_filetime_100ns=str(CREATION))
            clock.value=500;timeline.mark('play',owner='fresh',pid=43,creation_filetime_100ns=str(CREATION+1))
            clock.value=600;timeline.mark('pause',owner='fresh',pid=43,creation_filetime_100ns=str(CREATION+1))
            before=copy.deepcopy((first.result,second.result));clock.value=800;result=timeline.finish()
            self.assertTrue(result['recording_passed']);self.assertTrue(result['within_evidence_byte_limit'])
            self.assertEqual(result['phase_report']['closed_ns'],800)
            self.assertEqual([r['association']['assigned_count'] for r in result['owners']],[2,2])
            self.assertEqual([r['association']['assignments'][0]['phase'] for r in result['owners']],['play','play'])
            self.assertEqual((first.result,second.result),before)
            self.assertEqual(result['referenced_memory_report_bytes'],sum(p.stat().st_size for p in Path(directory).iterdir()))
            self.assertEqual(result['serialized_evidence_bytes'],len((json.dumps({'memory_timeline':result},indent=2,allow_nan=False)+'\n').encode()))

    def test_missing_ram_file_keeps_partial_rows_and_fails_requested_recording(self):
        timeline,clock=self.timeline()
        with tempfile.TemporaryDirectory() as directory:
            recorder=self.finished_sampler(directory,write=False);before=copy.deepcopy(recorder.result)
            timeline.attach('soak',recorder);timeline.mark('play',owner='soak',pid=42,creation_filetime_100ns=str(CREATION))
            clock.value=400;result=timeline.finish()
            self.assertFalse(result['recording_passed']);self.assertEqual(recorder.result,before)
            self.assertEqual(result['owners'][0]['partial_memory_report'],before)
            self.assertEqual(result['omissions'],[dict(owner='soak',reason='ram_report_file_unavailable')])
            self.assertEqual(result['owners'][0]['association']['sample_count'],2)

    def test_valid_partial_error_report_is_retained_and_associated_without_success(self):
        timeline,clock=self.timeline()
        with tempfile.TemporaryDirectory() as directory:
            recorder=self.finished_sampler(directory,write=False)
            recorder.result.update(stop_reason='error',error=dict(operation='GetProcessMemoryInfo',win32_error=5))
            soak.json_write(recorder.path,recorder.result);before=recorder.path.read_bytes()
            timeline.attach('soak',recorder);timeline.mark('play',owner='soak',pid=42,creation_filetime_100ns=str(CREATION))
            clock.value=400;result=timeline.finish()
            self.assertFalse(result['recording_passed']);self.assertEqual(recorder.path.read_bytes(),before)
            self.assertEqual(result['owners'][0]['association']['sample_count'],2)
            self.assertIn('memory_recorder',[r['operation'] for r in result['errors']])

    def test_one_row_early_interrupt_is_retained_but_not_recording_success(self):
        timeline,clock=self.timeline()
        with tempfile.TemporaryDirectory() as directory:
            partial=actual_report(samples=1);path=Path(directory)/'memory.json'
            soak.json_write(path,partial);before=path.read_bytes()
            recorder=SimpleNamespace(result=partial,path=path,identity=CREATION,
                thread=SimpleNamespace(is_alive=lambda:False))
            timeline.attach('soak',recorder)
            timeline.mark('play',owner='soak',pid=42,creation_filetime_100ns=str(CREATION))
            clock.value=400;result=timeline.finish()
            self.assertFalse(result['recording_passed']);self.assertEqual(path.read_bytes(),before)
            self.assertEqual(partial['sample_count'],1);self.assertEqual(len(partial['samples']),1)
            self.assertEqual(result['owners'][0]['association']['sample_count'],1)
            self.assertEqual(result['owners'][0]['association']['assigned_count'],1)
            self.assertIn('memory_recorder',[r['operation'] for r in result['errors']])

    def test_phase_failure_retains_trace_and_bounded_errors(self):
        timeline,clock=self.timeline();timeline.mark('startup',owner='soak')
        timeline.mark('/private-user-path',owner='soak')
        for _ in range(30):timeline.error('mark',ValueError('private-user-path'))
        clock.value=400;result=timeline.finish()
        self.assertFalse(result['recording_passed']);self.assertEqual(result['phase_report']['event_count'],1)
        self.assertEqual(len(result['errors']),16);self.assertEqual(result['errors_omitted'],result['error_count']-16)
        self.assertNotIn('private-user-path',json.dumps(result))

    def test_evidence_cap_preserves_ram_files_with_explicit_omission(self):
        timeline,clock=self.timeline()
        with tempfile.TemporaryDirectory() as directory:
            recorder=self.finished_sampler(directory);before=recorder.path.read_bytes()
            timeline.attach('soak',recorder);timeline.mark('play',owner='soak',pid=42,creation_filetime_100ns=str(CREATION))
            clock.value=400
            with patch.object(soak,'MAX_TIMELINE_BYTES',1024):result=timeline.finish()
            self.assertFalse(result['recording_passed']);self.assertIsNone(result['owners'][0]['association'])
            self.assertEqual(recorder.path.read_bytes(),before)
            self.assertIn('association_or_partial_report_byte_limit',[r['reason'] for r in result['omissions']])

    def test_finalize_requested_failure_changes_all_verdicts_preserving_native_error(self):
        timeline,clock=self.timeline();timeline.mark('startup',owner='soak');clock.value=400
        record=dict(passed=True,functional_reliability_passed=True,harness_rehearsal_passed=True,error='original-native-error')
        soak.finalize_memory_timeline(record,timeline)
        self.assertEqual(record['error'],'original-native-error');self.assertFalse(record['passed'])
        self.assertFalse(record['functional_reliability_passed']);self.assertFalse(record['harness_rehearsal_passed'])
        self.assertFalse(record['memory_timeline']['recording_passed'])

    def test_finalize_exception_is_bounded_and_cannot_overwrite_original_error(self):
        timeline,clock=self.timeline();clock.value=400
        timeline.finish=lambda:(_ for _ in ()).throw(ValueError('private-user-path'))
        record=dict(passed=True,functional_reliability_passed=True,harness_rehearsal_passed=False,error='original-native-error')
        soak.finalize_memory_timeline(record,timeline)
        self.assertFalse(record['passed']);self.assertEqual(record['error'],'original-native-error')
        self.assertEqual(record['memory_timeline']['errors'],[dict(operation='finalize',exception='ValueError')])
        self.assertNotIn('private-user-path',json.dumps(record))

    def test_inherited_diagnostics_snapshot_forwards_default_and_restore_phase(self):
        spec=importlib.util.spec_from_file_location('poima_timeline_wrapper_test',
            ROOT/'tests/performance_yard_reliability_diagnostics.py')
        module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
        game=object.__new__(module.DiagnosticReliability)
        game.label='fresh';game.replacements=1;game.selected=set();game.observed_checkpoints=set()
        reads=[];game.read_diagnostics=lambda label:reads.append(label)
        state={'tick':41,'complete_payload':'unmodified'}
        with patch.object(module.soak.Soak,'snapshot',autospec=True,return_value=state) as parent:
            self.assertIs(game.snapshot(),state)
            self.assertIs(game.snapshot(phase='restore_check'),state)
        self.assertEqual(parent.call_args_list,[( (game,),{'phase':'snapshot'}),
                                               ( (game,),{'phase':'restore_check'})])
        self.assertEqual(reads,['paused-restored-checkpoint-1'])


if __name__=='__main__':unittest.main()
