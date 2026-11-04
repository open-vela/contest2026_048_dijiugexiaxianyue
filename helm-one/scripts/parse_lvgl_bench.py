#!/usr/bin/env python3
"""Extract LVGL benchmark scene measurements from captured serial logs as CSV."""
import argparse
import csv
from pathlib import Path
import re
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    args = parser.parse_args()
    ansi = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')
    row = re.compile(r'(?:\[LVGL\]\s*)?([^\r\n,]+),\s*(\d+)%,\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*$')
    writer = csv.writer(sys.stdout)
    writer.writerow(('run', 'scene', 'cpu_pct', 'fps', 'total_ms', 'render_ms', 'flush_ms'))
    for path in args.logs:
        text = ansi.sub('', path.read_text(errors='replace'))
        if 'Benchmark Summary' not in text:
            parser.error(f'{path}: no benchmark summary')
        text = text[text.rfind('Benchmark Summary'):]
        found = []
        for line in text.splitlines():
            # Drop syslog prefixes before matching the CSV scene name.
            line = line.split('[LVGL]')[-1].strip()
            match = row.fullmatch(line)
            if match:
                found.append(match.groups())
        if len(found) != 17 or found[-1][0] != 'All scenes avg.':
            parser.error(f'{path}: incomplete summary ({len(found)}/17 rows)')
        writer.writerows((path.stem, *values) for values in found)


if __name__ == '__main__':
    main()
