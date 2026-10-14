# SPDX-License-Identifier: Apache-2.0
"""Actual shared-player UI observation assertions, used by the native UI test.

No layout recreation, fake server, native input, image analysis or simulation
writes. Run through shared_player_ui_contract.py with the normal owned gate.
"""
import math
import time
import uuid



def verify_readiness(owner):
    state = owner['readiness']
    assert state is not None and state['format'] == 'poima.player-readiness.v1', owner
    flags = ('backend_available','initialized','closing','finished','replay','owner_paused',
             'interactive_active','ui_ready','ui_presented','ui_observation_retained','ui_scale_pending')
    assert all(type(state[key]) is bool for key in flags), state
    assert state['focused'] is None or type(state['focused']) is bool
    assert (state['focused'] is None) == (state['focus_observation'] is None)
    assert state['focus_observation'] in (None,'initial_window_flags','focus_gained_event','focus_lost_event','minimized_event')
    assert state['drawable'] is None or type(state['drawable']) is bool
    assert (state['drawable'] is None) == (state['drawable_size'] is None)
    assert state['drawable_observation'] == (None if state['drawable'] is None else 'last_ordinary_frame_check')
    if state['drawable_size'] is not None:
        assert len(state['drawable_size']) == 2 and all(type(v) is int for v in state['drawable_size'])
    expected_ready = [name for name, blocked in (
        ('backend_unavailable', not state['backend_available']), ('not_initialized', not state['initialized']),
        ('no_current_presentation', not state['ui_presented']), ('closing', state['closing']), ('finished', state['finished'])) if blocked]
    expected_pause = [name for name, blocked in (
        ('owner_pause', state['owner_paused']),
        ('interactive_inactive', not state['replay'] and state['initialized'] and not state['interactive_active'])) if blocked]
    assert state['ready_conditions'] == expected_ready and state['pause_conditions'] == expected_pause, state
    assert owner['ready'] == (owner['active'] and not expected_ready) and owner['paused'] == bool(expected_pause), owner
    assert state['replay'] == (owner['mode'] == 'replay')
    if owner['ready'] and owner['report'] is not None:
        assert owner['report']['detail'] != 'Native player awaits its first owner poll.', owner
    if not owner['active']:
        assert state['finished'], owner
    return state

def verify(client, boundary, *, guards=False):
    owner, runtime, ui = (boundary[k] for k in ('owner', 'runtime', 'ui'))
    assert owner['paused'] and owner['ready'], 'Require an actually ready paused player'
    verify_readiness(owner)
    request = {k: owner[k] for k in ('player_id', 'generation')}
    request.update(session_id=owner['current_session_id'], tick=runtime['tick'], ui_revision=ui['ui_revision'])
    actual = client.call('player.ui.inspect', request)
    assert actual['format'] == 'poima.player-ui-observation.v1'
    assert all(actual[k] == request[k] for k in request)
    displayed = actual['displayed']
    assert displayed is not None and displayed['matches_current_owner'], actual
    assert displayed['session_id'] == request['session_id'] and displayed['tick'] == request['tick']
    assert displayed['ui_revision'] == request['ui_revision'] and displayed['player_generation'] == request['generation']
    assert displayed['frame_sequence'] > 0 and displayed['submission_id'] > 0
    assert 1 <= displayed['width'] <= 8192 and 1 <= displayed['height'] <= 8192
    assert .25 <= displayed['scale'] <= 8 and displayed['layout_generation'] > 0
    controls = displayed['controls']
    expected = {row['id']: row for row in ui['elements'] if row['kind'] != 'panel'}
    assert len(controls) <= 256 and len({c['id'] for c in controls}) == len(controls)
    assert set(expected) == {c['id'] for c in controls}
    for row in controls:
        assert row['kind'] == expected[row['id']]['kind']
        assert all(len(row[k]) == 4 and all(type(v) in (int, float) and math.isfinite(v) for v in row[k])
                   for k in ('bounds', 'clip'))
        assert type(row['font_size_px']) in (int, float) and math.isfinite(row['font_size_px']) and row['font_size_px'] > 0
        assert all(type(row[k]) is bool for k in ('visible', 'enabled', 'hittable', 'focused'))
        assert not row['hittable'] or row['kind'] == 'button' and row['enabled'] and row['visible']
    if guards:
        for key, value, code in [('player_id', uuid.uuid4().hex, -32004),
                                  ('generation', request['generation'] + 1, -32009),
                                  ('session_id', uuid.uuid4().hex, -32009),
                                  ('tick', request['tick'] + 1, -32009),
                                  ('ui_revision', request['ui_revision'] + 1, -32009),
                                  ('generation', True, -32602),
                                  ('tick', True, -32602),
                                  ('ui_revision', True, -32602)]:
            client.reject('player.ui.inspect', dict(request, **{key: value}), code)
        for key in ('player_id', 'generation', 'session_id'):
            missing = dict(request); missing.pop(key)
            client.reject('player.ui.inspect', missing, -32602)
        for omitted in (('tick',), ('ui_revision',), ('tick', 'ui_revision')):
            sparse = {key: value for key, value in request.items() if key not in omitted}
            observed = client.call('player.ui.inspect', sparse)
            assert all(observed[key] == request[key] for key in request), observed
            assert observed['displayed'] is not None and observed['displayed']['matches_current_owner']
        client.reject('player.ui.inspect', dict(request, scale=1), -32602)
    # Native owner maintenance may present paused frames between calls. It must
    # not invoke gameplay, commit logical UI, or advance simulation to answer.
    after = client.call('player.inspect')
    for key in ('player_id', 'generation', 'control_revision', 'current_session_id',
                'tick', 'ui_revision', 'control_sequence', 'structure_revision'):
        assert after[key] == owner[key], ('Read mutated owner', key, owner, after)
    assert client.call('runtime.inspect', {'session_id': request['session_id']}) == runtime
    assert client.call('runtime.ui.inspect', {'session_id': request['session_id'], 'tick': request['tick']}) == ui
    assert client.call('runtime.gameplay.inspect', {'session_id': request['session_id']}) == boundary['game']
    return actual


def verify_live_reads(client, *, require_unpaused=False):
    """Read after compiled Resume, without prior Tick/UI guards or a pause.

    Actual focus is external. Record what the native owner reports; only claim
    unpaused observation when real unpaused owners and advancing Ticks exist.
    --require-unpaused-ui turns missing OS focus/progress into an explicit test
    failure rather than silently qualifying a paused read as a running read.
    """
    reads = []
    for _ in range(32):
        before = client.call('player.inspect')
        assert before['active'], 'Live-read interval lost the native player'
        request = {key: before[key] for key in ('player_id', 'generation')}
        request['session_id'] = before['current_session_id']
        observed = client.call('player.ui.inspect', request)
        after = client.call('player.inspect')
        verify_readiness(before); verify_readiness(after)
        assert observed['format'] == 'poima.player-ui-observation.v1'
        assert all(observed[key] == request[key] for key in request)
        assert all(after[key] == before[key] for key in ('player_id', 'generation', 'current_session_id'))
        assert before['tick'] <= observed['tick'] <= after['tick']
        assert before['ui_revision'] <= observed['ui_revision'] <= after['ui_revision']
        displayed = observed['displayed']
        if displayed is not None:
            assert displayed['session_id'] == request['session_id']
            assert displayed['player_generation'] == request['generation']
            assert displayed['tick'] <= observed['tick'] and displayed['ui_revision'] <= observed['ui_revision']
            if displayed['matches_current_owner']:
                assert displayed['tick'] == observed['tick'] and displayed['ui_revision'] == observed['ui_revision']
        reads.append({'before': before, 'request': request, 'observation': observed, 'after': after})
        time.sleep(.01)  # External observation pacing; no input/step/focus fabrication.
    unpaused = [item for item in reads if not item['before']['paused'] and not item['after']['paused']]
    progressing = bool(unpaused) and unpaused[-1]['after']['tick'] > unpaused[0]['before']['tick']
    result = {'reads': reads, 'unpaused_reads': len(unpaused),
              'unpaused_advancing_observation_qualified': progressing,
              'scope': 'Actual native lifecycle and Tick observations; no physical-input, focus synthesis or frame-time qualification'}
    if require_unpaused:
        assert progressing, ('No genuinely unpaused advancing owner was observed; keep its window focused', result)
    return result
