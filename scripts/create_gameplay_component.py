#!/usr/bin/env python3
"""Create a C# component declaration with fresh persistent type and field IDs."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
import math
from pathlib import Path
import re
import struct
import uuid

KINDS = {'int32': 'int', 'int64': 'long', 'float32': 'float', 'float64': 'double', 'entity': 'global::Poima.EntityId'}
ZERO = '0' * 32


def identifier(value, role):
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{0,63}', value):
        raise ValueError(f'{role} must be an ASCII identifier of 1–64 characters.')
    return value


def initial(kind, text):
    if len(text) > 128:
        raise ValueError('A field default exceeds 128 characters.')
    if kind in ('int32', 'int64'):
        if not re.fullmatch(r'0|-?[1-9][0-9]*', text):
            raise ValueError(f'{kind} defaults must be canonical decimal integers.')
        bits = 32 if kind == 'int32' else 64
        value = int(text)
        if not -(2**(bits-1)) <= value < 2**(bits-1):
            raise ValueError(f'{kind} default is outside its signed range.')
        return str(value)
    if kind in ('float32', 'float64'):
        if not re.fullmatch(r'[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?', text):
            raise ValueError(f'{kind} default must be an invariant decimal number.')
        value = float(text)
        if not math.isfinite(value):
            raise ValueError(f'{kind} default must be finite.')
        if kind == 'float32':
            try:
                value = struct.unpack('<f', struct.pack('<f', value))[0]
            except OverflowError as error:
                raise ValueError('float32 default is outside its finite range.') from error
        return '0' if value == 0 else format(value, '.9g' if kind == 'float32' else '.17g')
    if text != ZERO:
        raise ValueError('Entity defaults must be the all-zero unset ID.')
    return ZERO


def declaration(name, namespace, specifications):
    identifier(name, 'Component name')
    if len(namespace) > 256:
        raise ValueError('Namespace exceeds 256 characters.')
    segments = [identifier(part, 'Namespace segment') for part in namespace.split('.')]
    if not 1 <= len(specifications) <= 32:
        raise ValueError('A component requires 1–32 fields.')
    fields = []
    seen = set()
    for specification in specifications:
        parts = specification.split(':', 2)
        if len(parts) != 3:
            raise ValueError('Use --field Name:kind:default (for example Current:float32:100).')
        field, kind, default = parts
        identifier(field, 'Field name')
        if field == name or field in seen:
            raise ValueError('Field names must be unique and differ from the component name.')
        if kind not in KINDS:
            raise ValueError('Field kind must be one of: '+', '.join(KINDS))
        seen.add(field)
        fields.append((field, kind, initial(kind, default), uuid.uuid4().hex))
    component_id = uuid.uuid4().hex
    # Verbatim identifiers handle C# keywords; JSON-quoted scalar attribute
    # strings are also valid C# escaped literals. Defaults cannot contain code.
    lines = ['// SPDX-License-Identifier: Apache-2.0',
             '// Keep these IDs when renaming fields or the component. Generate a new declaration only for a new type.',
             'using global::Poima;', '', 'namespace '+'.'.join('@'+part for part in segments)+';', '',
             f'[global::Poima.GameplayComponent({json.dumps(component_id)})]', f'public partial struct @{name}', '{']
    for field, kind, default, field_id in fields:
        lines += [f'    [global::Poima.GameplayField({json.dumps(field_id)}, Default = {json.dumps(default)})]',
                  f'    public {KINDS[kind]} @{field};', '']
    lines[-1] = '}'
    return '\n'.join(lines)+'\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True, help='Component type name (ASCII C# identifier).')
    parser.add_argument('--namespace', required=True, help='Dot-separated C# namespace.')
    parser.add_argument('--output', type=Path, required=True, help='New .cs file in an existing directory; never overwritten.')
    parser.add_argument('--field', action='append', help='Name:kind:default; repeat up to32 times. Kinds: '+', '.join(KINDS))
    args = parser.parse_args()
    try:
        if args.output.suffix.lower() != '.cs':
            raise ValueError('Output must have a .cs extension.')
        code = declaration(args.name, args.namespace, args.field if args.field is not None else ['Value:float32:0'])
        with args.output.open('x', encoding='utf-8', newline='\n') as output:
            output.write(code)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(args.output)


if __name__ == '__main__':
    main()
