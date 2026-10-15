#!/usr/bin/env python3
"""Portable integer-clock association tests; no native/Windows API calls."""
# SPDX-License-Identifier: Apache-2.0
import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest


def load(name, filename):
    path=Path(__file__).resolve().parents[1]/'tools/performance'/filename
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);sys.modules[name]=module
    spec.loader.exec_module(module)
    return module


phases=load('poima_memory_phases','memory_phases.py')
sampler=load('poima_memory_phases_sampler','windows_process_memory.py')
CREATION='134000000000000001'


class Clock:
    def __init__(self,value=100):self.value=value;self.reads=0
    def __call__(self):self.reads+=1;return self.value


def trace(*, owner='host', pid=42, creation=CREATION):
    clock=Clock();log=phases.PhaseLog(100,clock_ns=clock)
    clock.value=200;log.mark('play',owner=owner,pid=pid,creation_filetime_100ns=creation)
    clock.value=400;log.mark('pause',owner=owner,pid=pid,creation_filetime_100ns=creation)
    clock.value=600;log.close()
    return log.snapshot()


def memory(clock, intervals, *, pid=42, creation=CREATION):
    # Deliberately unrelated elapsed floats: association must use integer brackets.
    return dict(format='poima.windows-process-memory',version=2,completed=True,
        clock=copy.deepcopy(clock),sampler_started_ns=clock['origin_ns'],
        process=dict(pid=pid,creation_filetime_100ns=creation),
        samples=[dict(elapsed_seconds=9999.+i,working_set_bytes=100+i,
            private_commit_bytes=200+i,query_begin_ns=a,query_end_ns=b)
            for i,(a,b) in enumerate(intervals)])


class PhaseLogContracts(unittest.TestCase):
    def test_exact_observed_tags_clock_and_detached_snapshots(self):
        clock=Clock(120);log=phases.PhaseLog(100,clock_ns=clock)
        event=log.mark('startup',owner='host-main',pid=42,creation_filetime_100ns=CREATION,
            player_id='1'*32,generation=2,session_id='2'*32,tick=7,runtime_replacements=3,cycle=4)
        self.assertEqual(event,dict(sequence=1,at_ns=120,phase='startup',owner='host-main',
            pid=42,creation_filetime_100ns=CREATION,player_id='1'*32,generation=2,
            session_id='2'*32,tick=7,runtime_replacements=3,cycle=4))
        value=log.snapshot()
        self.assertEqual(value['clock'],dict(domain='python.time.monotonic_ns',
            collector_pid=phases.os.getpid(),origin_ns=100))
        self.assertEqual(value['format'],'poima.memory-phases');self.assertEqual(value['version'],1)
        self.assertFalse(value['completed']);self.assertIsNone(value['closed_ns'])
        event['tick']=99;value['events'][0]['owner']='changed';value['clock']['origin_ns']=99
        value['limits']['maximum_events']=0
        fresh=log.snapshot();self.assertEqual(fresh['events'][0]['tick'],7)
        self.assertEqual(fresh['events'][0]['owner'],'host-main');self.assertEqual(fresh['clock']['origin_ns'],100)

    def test_nullable_observations_and_closed_horizon_are_not_new_phases(self):
        clock=Clock(120);log=phases.PhaseLog(100,clock_ns=clock)
        log.mark('cleanup',owner='client')
        for key in ('pid','creation_filetime_100ns','player_id','generation','session_id','tick','runtime_replacements','cycle'):
            self.assertIsNone(log.snapshot()['events'][0][key])
        clock.value=200;log.close();reads=clock.reads;log.close()
        final=log.snapshot();self.assertTrue(final['completed']);self.assertEqual(final['closed_ns'],200)
        self.assertEqual(final['event_count'],1);self.assertEqual(clock.reads,reads)
        with self.assertRaises(ValueError):log.mark('play',owner='client')
        self.assertEqual(log.snapshot()['event_count'],1)

    def test_invalid_tags_preserve_valid_prefix_and_bounded_error(self):
        bad=[dict(phase='/private-user-path'),dict(owner='../secret'),dict(owner='Höst'),dict(owner='x'*65),
             dict(pid=True),dict(pid=0),dict(pid=2**32),dict(creation_filetime_100ns=1),
             dict(creation_filetime_100ns='01'),dict(creation_filetime_100ns=str(2**64)),
             dict(player_id='/private-user-path'),dict(session_id=True),dict(generation=-1),
             dict(tick=True),dict(runtime_replacements=2**64),dict(cycle=1.5)]
        for change in bad:
            with self.subTest(change=change):
                clock=Clock(120);log=phases.PhaseLog(100,clock_ns=clock)
                first=log.mark('startup',owner='host');params=dict(phase='play',owner='host');params.update(change)
                clock.value=130
                with self.assertRaises(ValueError):log.mark(**params)
                clock.value=140;log.close();snapshot=log.snapshot()
                self.assertFalse(snapshot['completed']);self.assertEqual(snapshot['events'],[first])
                self.assertEqual(set(snapshot['error']),{'operation'})
                self.assertNotIn('private-user-path',json.dumps(snapshot))

    def test_clock_failure_regression_and_overflow_do_not_repair_history(self):
        for bad in (119,True,-1,2**64,120.,float('nan')):
            with self.subTest(bad=bad):
                clock=Clock(120);log=phases.PhaseLog(100,clock_ns=clock);event=log.mark('play',owner='host')
                clock.value=bad
                with self.assertRaises(ValueError):log.mark('pause',owner='host')
                snapshot=log.snapshot();self.assertEqual(snapshot['events'],[event]);self.assertFalse(snapshot['completed'])
                json.dumps(snapshot,allow_nan=False)
        def failed():raise OSError('private-user-path')
        log=phases.PhaseLog(100,clock_ns=failed)
        with self.assertRaises(ValueError):log.mark('play',owner='host')
        self.assertNotIn('private-user-path',json.dumps(log.snapshot()))

    def test_close_regression_retains_trace_without_inventing_horizon(self):
        clock=Clock(200);log=phases.PhaseLog(100,clock_ns=clock);event=log.mark('play',owner='host')
        clock.value=199
        with self.assertRaises(ValueError):log.close()
        snapshot=log.snapshot();self.assertFalse(snapshot['completed']);self.assertIsNone(snapshot['closed_ns'])
        self.assertEqual(snapshot['events'],[event])

    def test_event_and_byte_limits_never_silently_evict(self):
        clock=Clock(120);log=phases.PhaseLog(100,clock_ns=clock,max_events=2)
        first=log.mark('startup',owner='host');second=log.mark('play',owner='host')
        with self.assertRaises(ValueError):log.mark('pause',owner='host')
        snapshot=log.snapshot();self.assertEqual(snapshot['events'],[first,second])
        self.assertEqual(snapshot['error']['operation'],'event_limit');self.assertEqual(snapshot['rejected_events'],1)
        log=phases.PhaseLog(100,clock_ns=clock,max_events=20,max_bytes=2048);accepted=[]
        for _ in range(20):
            try:accepted.append(log.mark('play',owner='host',pid=42,creation_filetime_100ns=CREATION))
            except ValueError:break
        snapshot=log.snapshot();self.assertGreater(len(accepted),0);self.assertLess(len(accepted),20)
        self.assertEqual(snapshot['events'],accepted);self.assertEqual(snapshot['error']['operation'],'byte_limit')
        self.assertLessEqual(len(json.dumps(snapshot,sort_keys=True,separators=(',',':')).encode()),2048)

    def test_invalid_constructor_bounds(self):
        for origin in (True,-1,2**64,1.0):
            with self.subTest(origin=origin),self.assertRaises(ValueError):phases.PhaseLog(origin)
        for kwargs in (dict(max_events=0),dict(max_events=True),dict(max_events=4097),
                       dict(max_bytes=1023),dict(max_bytes=1048577),dict(clock_ns=42)):
            with self.subTest(kwargs=kwargs),self.assertRaises(ValueError):phases.PhaseLog(0,**kwargs)


class AssociationContracts(unittest.TestCase):
    def classify(self,intervals,**kwargs):
        report=trace(**kwargs);return phases.classify_samples(memory(report['clock'],intervals),report,owner='host')

    def test_whole_query_containment_and_touching_boundaries(self):
        value=self.classify([(100,150),(200,250),(350,400),(400,450),(550,600),(600,610)])
        self.assertTrue(value['supported']);self.assertEqual((value['sample_count'],value['assigned_count'],value['unassigned_count']),(6,2,4))
        self.assertEqual(value['assignments'],[
            dict(sample=0,phase_sequence=None,phase=None,reason='before_first_phase'),
            dict(sample=1,phase_sequence=1,phase='play',reason='within_phase'),
            dict(sample=2,phase_sequence=None,phase=None,reason='crosses_phase_boundary'),
            dict(sample=3,phase_sequence=2,phase='pause',reason='within_phase'),
            dict(sample=4,phase_sequence=None,phase=None,reason='after_trace_horizon'),
            dict(sample=5,phase_sequence=None,phase=None,reason='after_trace_horizon')])
        self.assertEqual(self.classify([(350,450)])['assignments'][0]['reason'],'crosses_phase_boundary')

    def test_duplicate_boundaries_are_ambiguous_not_repaired(self):
        clock=Clock(200);log=phases.PhaseLog(100,clock_ns=clock)
        for phase in ('warmup','play'):log.mark(phase,owner='host',pid=42,creation_filetime_100ns=CREATION)
        clock.value=400;log.close();report=log.snapshot()
        for interval in ((200,220),(180,200),(200,200)):
            result=phases.classify_samples(memory(report['clock'],[interval]),report,owner='host')
            self.assertEqual(result['assigned_count'],0);self.assertEqual(result['assignments'][0]['reason'],'ambiguous_boundary')
        self.assertEqual([r['at_ns'] for r in report['events']],[200,200])

    def test_owner_pid_and_creation_identity_all_must_match(self):
        for kwargs,reason in [(dict(owner='client'),'owner_mismatch'),(dict(pid=43),'process_identity_mismatch'),
                              (dict(creation=str(int(CREATION)+1)),'process_identity_mismatch'),
                              (dict(pid=None),'process_identity_unavailable'),(dict(creation=None),'process_identity_unavailable')]:
            with self.subTest(kwargs=kwargs):
                result=self.classify([(210,250)],**kwargs)
                self.assertEqual(result['assigned_count'],0);self.assertEqual(result['assignments'][0]['reason'],reason)

    def test_clock_mismatch_incomplete_and_legacy_are_explicitly_unsupported(self):
        report=trace();base=memory(report['clock'],[(210,250)])
        for key,value in [('origin_ns',101),('collector_pid',report['clock']['collector_pid']+1)]:
            with self.subTest(key=key):
                changed=copy.deepcopy(base);changed['clock'][key]=value
                if key=='origin_ns':changed['sampler_started_ns']=value
                result=phases.classify_samples(changed,report,owner='host')
                self.assertFalse(result['supported']);self.assertEqual(result['assignments'][0]['reason'],'clock_mismatch')
        incomplete=copy.deepcopy(report);incomplete['completed']=False
        result=phases.classify_samples(base,incomplete,owner='host')
        self.assertFalse(result['supported']);self.assertEqual(result['assignments'][0]['reason'],'phase_trace_incomplete')
        legacy=copy.deepcopy(base);legacy['version']=1;del legacy['clock'];del legacy['sampler_started_ns']
        for row in legacy['samples']:del row['query_begin_ns'];del row['query_end_ns']
        result=phases.classify_samples(legacy,report,owner='host')
        self.assertFalse(result['supported']);self.assertEqual(result['assignments'][0]['reason'],'legacy_without_query_clock')

    def test_empty_closed_trace_does_not_guess_a_phase(self):
        clock=Clock(600);log=phases.PhaseLog(100,clock_ns=clock);log.close();report=log.snapshot()
        result=phases.classify_samples(memory(report['clock'],[(210,250)]),report,owner='host')
        self.assertTrue(result['supported']);self.assertEqual(result['assigned_count'],0)
        self.assertEqual(result['assignments'][0]['reason'],'before_first_phase')

    def test_inputs_and_detached_result_remain_immutable(self):
        report=trace();sample=memory(report['clock'],[(210,250)]);before=copy.deepcopy((sample,report))
        result=phases.classify_samples(sample,report,owner='host');result['assignments'][0]['phase']='changed'
        self.assertEqual((sample,report),before)

    def test_invalid_typed_clocks_intervals_and_trace_shape_reject(self):
        report=trace();sample=memory(report['clock'],[(210,250)])
        bad_samples=[]
        for key,value in [('query_begin_ns',True),('query_end_ns',250.),('query_end_ns',209),
                          ('query_begin_ns',99),('query_end_ns',2**64),('working_set_bytes',True)]:
            changed=copy.deepcopy(sample);changed['samples'][0][key]=value;bad_samples.append(changed)
        changed=memory(report['clock'],[(210,250),(240,270)]);bad_samples.append(changed)
        for changed in bad_samples:
            with self.subTest(changed=changed),self.assertRaises(ValueError):phases.classify_samples(changed,report,owner='host')
        for change in ('sequence','clock_regression','horizon','count','clock_type','rejected_bool'):
            altered=copy.deepcopy(report)
            if change=='sequence':altered['events'][0]['sequence']=True
            elif change=='clock_regression':altered['events'][1]['at_ns']=199
            elif change=='horizon':altered['closed_ns']=399
            elif change=='count':altered['event_count']=1
            elif change=='clock_type':altered['clock']['origin_ns']=100.
            else:altered['rejected_events']=False
            with self.subTest(change=change),self.assertRaises(ValueError):phases.classify_samples(sample,altered,owner='host')

    def test_unsupported_trace_still_requires_bounded_valid_partial_schema(self):
        report=trace();sample=memory(report['clock'],[(210,250)])
        for mode in ('incomplete','other_clock'):
            for field,value in [('events','not-an-event-array'),('event_count',True),
                                ('events',[dict(report['events'][0],at_ns=-1)])]:
                with self.subTest(mode=mode,field=field):
                    changed=copy.deepcopy(report);changed[field]=value
                    if mode=='incomplete':changed['completed']=False
                    else:changed['clock']['collector_pid']+=1
                    with self.assertRaises(ValueError):phases.classify_samples(sample,changed,owner='host')

    def test_failed_start_clock_is_unsupported_without_backdating_rows(self):
        def invalid_clock():return -1
        report=sampler.collect(sampler.Options(42,query_timing=True),monotonic_ns=invalid_clock,
            provider_factory=lambda _:self.fail('Invalid start clock must not open provider'))
        self.assertEqual(report['sample_count'],0);self.assertIsNone(report['clock']['origin_ns'])
        phases_report=trace();result=phases.classify_samples(report,phases_report,owner='host')
        self.assertFalse(result['supported']);self.assertEqual(result['sample_count'],0)
        self.assertEqual(result['assignments'],[])

    def test_sampler_start_is_typed_and_bounds_each_query(self):
        report=trace();sample=memory(report['clock'],[(210,250)])
        for value in (True,99,211,2**64,100.):
            with self.subTest(value=value):
                changed=copy.deepcopy(sample);changed['sampler_started_ns']=value
                with self.assertRaises(ValueError):phases.classify_samples(changed,report,owner='host')

    def test_failed_provider_open_retains_valid_empty_unknown_identity(self):
        def denied(_):raise sampler.ProbeError('OpenProcess',5)
        report=sampler.collect(sampler.Options(42,query_timing=True),provider_factory=denied,
            monotonic_ns=lambda:100,clock_origin_ns=100)
        self.assertEqual(report['samples'],[]);self.assertIsNone(report['process']['creation_filetime_100ns'])
        result=phases.classify_samples(report,trace(),owner='host')
        self.assertFalse(result['supported']);self.assertEqual(result['sample_count'],0)
        self.assertEqual(result['assignments'],[])

    def test_trace_declared_limits_cannot_be_lower_than_actual_trace(self):
        report=trace();sample=memory(report['clock'],[(210,250)])
        changed=copy.deepcopy(report);changed['limits']['maximum_events']=1
        with self.assertRaises(ValueError):phases.classify_samples(sample,changed,owner='host')
        clock=Clock(200);log=phases.PhaseLog(100,clock_ns=clock)
        for i in range(8):
            clock.value=200+i*10
            log.mark('play',owner='host',pid=42,creation_filetime_100ns=CREATION,
                player_id='1'*32,generation=1,session_id='2'*32,tick=i,cycle=1)
        clock.value=400;log.close();large=log.snapshot();large['limits']['maximum_bytes']=1024
        self.assertGreater(len(json.dumps(large,sort_keys=True,separators=(',',':')).encode()),1024)
        with self.assertRaises(ValueError):phases.classify_samples(sample,large,owner='host')

    def test_actual_slow_fake_provider_crosses_phase_even_when_elapsed_is_zero(self):
        clock=Clock(100);log=phases.PhaseLog(100,clock_ns=clock)
        log.mark('play',owner='host',pid=42,creation_filetime_100ns=CREATION)
        class Provider:
            creation_filetime=int(CREATION)
            closed=0
            def sample(self):
                clock.value=200;log.mark('pause',owner='host',pid=42,creation_filetime_100ns=CREATION)
                clock.value=210
                return dict(creation_filetime=self.creation_filetime,working_set_bytes=100,private_commit_bytes=200)
            def close(self):self.closed+=1
        provider=Provider();clock.value=110
        report=sampler.collect(sampler.Options(42,max_samples=1,query_timing=True),provider_factory=lambda _:provider,
            monotonic=lambda:0.,sleep=lambda _:self.fail('Single query must not sleep'),monotonic_ns=clock,clock_origin_ns=100)
        clock.value=300;log.close();result=phases.classify_samples(report,log.snapshot(),owner='host')
        self.assertEqual(report['samples'][0]['elapsed_seconds'],0.)
        self.assertEqual((report['samples'][0]['query_begin_ns'],report['samples'][0]['query_end_ns']),(110,210))
        self.assertEqual(result['assignments'][0]['reason'],'crosses_phase_boundary');self.assertEqual(result['assigned_count'],0)
        self.assertEqual(provider.closed,1)


if __name__=='__main__':unittest.main()
