#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Lower OIDN half arithmetic to IEEE conversion helpers and i16 storage."""
from pathlib import Path
import re
import sys

SSA = r'%(?:[-a-zA-Z$._0-9]+|"(?:[^"\\]|\\.)+")'
OPERAND = rf'(?:{SSA}|zeroinitializer|poison|undef)'


def lower_half(source):
    count = 0

    def decode(lanes, operand, prefix, lines):
        if operand in ('zeroinitializer', 'poison', 'undef'):
            return operand
        vector = 'poison'
        for lane in range(lanes):
            name = f'{prefix}_{lane}'
            lines.extend([
                f'{name}h = extractelement <{lanes} x half> {operand}, i32 {lane}',
                f'{name}z = zext i16 {name}h to i32',
                f'{name}f = call float @oidn_web_half_to_float(i32 {name}z)',
                f'{name}v = insertelement <{lanes} x float> {vector}, float {name}f, i32 {lane}',
            ])
            vector = name + 'v'
        return vector

    def cast(match):
        nonlocal count
        result, operation, lanes, operand = match[1], match[2], int(match[3]), match[4]
        prefix = f'%webhalf{count}'
        count += 1
        lines = []
        if operation == 'fpext':
            vector = decode(lanes, operand, prefix, lines)
            # Copy exactly, preserving negative zero and NaN payload bits.
            lines.append(f'{result} = select i1 true, <{lanes} x float> {vector}, <{lanes} x float> poison')
        else:
            vector = 'poison'
            for lane in range(lanes):
                name = f'{prefix}_{lane}'
                lines.extend([
                    f'{name}f = extractelement <{lanes} x float> {operand}, i32 {lane}',
                    f'{name}z = call i32 @oidn_web_float_to_half(float {name}f)',
                    f'{name}h = trunc i32 {name}z to i16',
                    f'{name}v = insertelement <{lanes} x half> {vector}, i16 {name}h, i32 {lane}',
                ])
                vector = name + 'v'
            lines.append(f'{result} = select i1 true, <{lanes} x half> {vector}, <{lanes} x half> poison')
        return '\n'.join('  ' + line for line in lines)

    source = re.sub(
        rf'(?m)^\s*({SSA}) = (fpext|fptrunc) <(\d+) x (?:half|float)> ({OPERAND}) to <\3 x (?:float|half)>\s*$',
        cast, source)

    def compare(match):
        nonlocal count
        result, predicate, lanes, left, right = match[1], match[2], int(match[3]), match[4], match[5]
        prefix = f'%webhalf{count}'
        count += 1
        lines = []
        left = decode(lanes, left, prefix + 'a', lines)
        right = decode(lanes, right, prefix + 'b', lines)
        lines.append(f'{result} = fcmp {predicate} <{lanes} x float> {left}, {right}')
        return '\n'.join('  ' + line for line in lines)

    source = re.sub(
        rf'(?m)^\s*({SSA}) = fcmp (\w+) <(\d+) x half> ({OPERAND}), ({OPERAND})\s*$',
        compare, source)
    if re.search(r'\b(?:fpext|fptrunc|fcmp|fadd|fsub|fmul|fdiv|frem|fneg)\b[^\n]*\bhalf\b', source):
        raise ValueError('Unsupported half operation in OIDN generic IR')
    if count:
        source += '\ndeclare float @oidn_web_half_to_float(i32)\ndeclare i32 @oidn_web_float_to_half(float)\n'
    # Half loads, stores, selects and masked gather/scatter carry bits only.
    source = re.sub(r'\bhalf\b', 'i16', source)
    return re.sub(r'(@llvm\.[\w.]+\.v\d+)f16(?=\.)', r'\1i16', source)


if __name__ == '__main__':
    Path(sys.argv[2]).write_text(lower_half(Path(sys.argv[1]).read_text()))
