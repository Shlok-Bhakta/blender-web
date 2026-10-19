#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Sample a Linux process tree's proportional memory without double-counting shared pages."""
import argparse
import json
import os
from pathlib import Path
import signal
import time


def sample(root):
    parents, stats = {}, {}
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            fields = (entry / 'stat').read_text().rsplit(')', 1)[1].split()
            parents[int(entry.name)] = int(fields[1])
            stats[int(entry.name)] = fields
        except (OSError, IndexError, ValueError):
            pass
    tree = {root}
    while True:
        expanded = tree | {pid for pid, parent in parents.items() if parent in tree}
        if expanded == tree:
            break
        tree = expanded
    totals = dict(rssKiB=0, pssKiB=0, privateKiB=0, processes=0, cpuSeconds=0, threads=0)
    for pid in tree:
        try:
            values = {}
            for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines():
                key, _, value = line.partition(':')
                if key in ('Rss', 'Pss', 'Private_Clean', 'Private_Dirty'):
                    values[key] = int(value.split()[0])
            totals['rssKiB'] += values['Rss']
            totals['pssKiB'] += values['Pss']
            totals['privateKiB'] += values.get('Private_Clean', 0) + values.get('Private_Dirty', 0)
            totals['processes'] += 1
            totals['cpuSeconds'] += (int(stats[pid][11]) + int(stats[pid][12])) / os.sysconf('SC_CLK_TCK')
            totals['threads'] += int(stats[pid][17])
        except (OSError, KeyError, ValueError):
            pass
    return dict(monotonic=time.monotonic(), **totals)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    running = True
    def stop(*_):
        nonlocal running
        running = False
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    with args.output.open('w') as output:
        while running and Path(f'/proc/{args.root}').exists():
            output.write(json.dumps(sample(args.root)) + '\n')
            output.flush()
            time.sleep(.2)


if __name__ == '__main__':
    main()
