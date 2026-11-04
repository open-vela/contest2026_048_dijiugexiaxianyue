#!/usr/bin/env python3
"""Switch an existing board build to LVGL benchmark, or restore its saved config."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--restore', action='store_true')
    parser.add_argument('--no-epic', action='store_true',
                        help='Disable the complete Sifli EPIC draw backend for isolation testing')
    parser.add_argument('--opt-level', choices=('keep', '-O1', '-O2'), default='keep')
    args = parser.parse_args()
    root = Path(__file__).absolute().parents[3]
    out = root / 'cmake_out/helmone_nsh'
    config = out / '.config'
    saved = out / '.config.pre-lvgl-bench'
    if not config.is_file():
        parser.error('Build the normal firmware once before switching to benchmark.')
    if args.restore:
        if not saved.is_file():
            parser.error(f'No saved production configuration: {saved}')
        shutil.copyfile(saved, config)
        if args.no_epic:
            text = config.read_text()
            for name in ('MYVENDOR_LVGL_EPIC_QUEUE_MODE',
                         'MYVENDOR_LVGL_SIFLI_EPIC', 'LV_USE_SIFLI_EPIC'):
                text = re.sub(rf'^(?:CONFIG_{name}=.*|# CONFIG_{name} is not set)\n',
                              '', text, flags=re.M)
                text += f'# CONFIG_{name} is not set\n'
            config.write_text(text)
    else:
        if 'CONFIG_MYVENDOR_LVGL_BENCH=y' in config.read_text():
            if not saved.exists():
                parser.error('Benchmark is already enabled but production backup is missing; restore the product config first.')
        else:
            shutil.copyfile(config, saved)
        changes = {
            'MYVENDOR_BICYCLE_AUTOSTART': 'n',
            'MYVENDOR_LVGL_BENCH': 'y',
            'MYVENDOR_LVGL_BENCH_PERF_MONITOR': 'y',
            'MYVENDOR_LCD_DISP_DEBUG': 'y',
            'LV_USE_PERF_MONITOR_LOG_MODE': 'n',
            'LV_DEF_REFR_PERIOD': '16',
            'USEC_PER_TICK': '1000',
            'RR_INTERVAL': '100',
        }
        if args.opt_level != 'keep':
            changes['LV_OPTLEVEL'] = f'"{args.opt_level}"'
        if args.no_epic:
            changes.update({
                'MYVENDOR_LVGL_EPIC_QUEUE_MODE': 'n',
                'MYVENDOR_LVGL_SIFLI_EPIC': 'n',
                'LV_USE_SIFLI_EPIC': 'n',
            })
        text = config.read_text()
        for name, value in changes.items():
            text = re.sub(rf'^(?:CONFIG_{name}=.*|# CONFIG_{name} is not set)\n', '', text, flags=re.M)
            text += f'# CONFIG_{name} is not set\n' if value == 'n' else f'CONFIG_{name}={value}\n'
        config.write_text(text)
    env = dict(os.environ, KCONFIG_CONFIG=str(config), EXTERNALDIR='dummy',
               APPSDIR=str(root / 'apps'), DRIVERS_PLATFORM_DIR='dummy',
               APPSBINDIR=str(out / 'apps'), BINDIR=str(out))
    subprocess.run(['olddefconfig'], cwd=root / 'nuttx', env=env, check=True)
    # Match the board's CMake menuconfig target: regenerate config.h and CMake.
    (out / 'include/nuttx/config.h').unlink(missing_ok=True)
    (root / 'nuttx/CMakeLists.txt').touch()
    if args.restore:
        print('Restored production config' + (' without EPIC.' if args.no_epic else '.'))
    else:
        print(('Benchmark config without EPIC' if args.no_epic else 'Benchmark config')
              + '; bicycle autostart disabled.')
    print('Next: ./vela_my_vendor_tools.py build flash monitor -p <current board port>')
    print(f'Production config backup: {saved}')


if __name__ == '__main__':
    main()
