"""Compare clocked receiver statuses against a stationary antenna's published ECEF position.

SPDX-License-Identifier: Apache-2.0
"""

import argparse
import json
from pathlib import Path

import numpy as np

from compare_ground import position, rows
from ground_fix import geodetic
from iqio import RATE


def fix_errors(status, antenna):
    lat, lon = np.radians(geodetic(*antenna)[:2])
    up = np.array((np.cos(lat)*np.cos(lon), np.cos(lat)*np.sin(lon), np.sin(lat)))
    east = np.array((-np.sin(lon), np.cos(lon), 0))
    north = np.cross(up, east)
    errors = {}
    previous = -1
    for row in status:
        sample = int(row['sample'])
        if sample <= previous or row['valid'] not in ('0', '1'):
            raise ValueError('expected ordered, unique clocked statuses with a validity flag')
        previous = sample
        if row['valid'] == '1':
            delta = position(row) - antenna
            errors[sample] = (int(row['nav_mask']), int(row['sv']),
                              float(delta @ up), float(np.hypot(delta @ east, delta @ north)))
    return errors


def summarize(fixes):
    if not fixes:
        return {'fixes': 0}
    samples = sorted(fixes)
    up = np.array([fixes[k][2] for k in samples])
    horizontal = np.array([fixes[k][3] for k in samples])
    return dict(fixes=len(fixes), first_s=samples[0] / RATE, last_s=samples[-1] / RATE,
                median_up_m=float(np.median(up)), p95_abs_up_m=float(np.percentile(abs(up), 95)),
                p95_horizontal_m=float(np.percentile(horizontal, 95)),
                p95_3d_m=float(np.percentile(np.hypot(up, horizontal), 95)))


def score(baseline, trial):
    common = sorted(k for k in baseline.keys() & trial.keys() if baseline[k][:2] == trial[k][:2])
    return dict(baseline=summarize(baseline), trial=summarize(trial),
                paired_baseline=summarize({k: baseline[k] for k in common}),
                paired_trial=summarize({k: trial[k] for k in common}))


if __name__ == '__main__':
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('baseline', type=Path)
    cli.add_argument('trial', type=Path)
    cli.add_argument('--ecef', type=float, nargs=3, required=True, metavar=('X', 'Y', 'Z'))
    args = cli.parse_args()
    antenna = np.array(args.ecef)
    print(json.dumps(score(*(fix_errors(rows(path), antenna) for path in (args.baseline, args.trial))), indent=2))
