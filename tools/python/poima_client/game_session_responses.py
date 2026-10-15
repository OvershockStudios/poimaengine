# SPDX-License-Identifier: Apache-2.0
"""Opt-in runtime-game-session v1 result checks over the packaged frozen schema.

This module sends no requests, retries no mutations and performs no implicit
schema lookup. Availability/error/behavior qualification is separate from
successful-result validation. Original params and trusted dynamic schemas are
needed for the corresponding request-dependent and typed-value checks.
"""
import json
from importlib import resources

from .runtime_response_checks_v1 import ResultChecksV1

_CHECKS = ResultChecksV1(json.loads(resources.files(__package__).joinpath(
    'runtime_game_session_responses.v1.json').read_text(encoding='utf-8')))


def game_session_response_contract():
    """Return a detached copy of the frozen nineteen-method v1 result contract."""
    return _CHECKS.contract()


def validate_game_session_result(method, result, params=None, context=None):
    """Return the original successful result or raise ResponseContractError.

    Validation does not prove a mutation failed or authorize a retry. Params are
    the original initiating request, including historical receipt guards; schema
    context is trusted metadata, not an inferred latest runtime boundary.
    """
    return _CHECKS.validate(method, result, params, context)
