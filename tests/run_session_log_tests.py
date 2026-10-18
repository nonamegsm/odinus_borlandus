#!/usr/bin/env python3
"""Build and run the session logger with the real Studio 15 classic BCC32."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

def quoted(path):
    return '"' + str(path) + '"'

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bds', default=r'C:\Program Files (x86)\Embarcadero\Studio\15.0')
    parser.add_argument('--configuration', choices=('Debug', 'Release', 'All'), default='All')
    args = parser.parse_args()
    bds = Path(args.bds)
    compiler = bds / 'bin/bcc32.exe'
    if not compiler.is_file():
        raise RuntimeError('Missing classic compiler: ' + str(compiler))
    env = os.environ.copy()
    env['PATH'] = str(bds / 'bin') + os.pathsep + env.get('PATH', '')
    results = []
    for configuration in (('Debug', 'Release') if args.configuration == 'All' else (args.configuration,)):
        out = ROOT / '.omx/research/2026-10-18-sync/log-tests' / configuration
        out.mkdir(parents=True, exist_ok=True)
        executable = out / 'session_log_tests.exe'
        includes = [ROOT / 'src', bds / 'include', bds / 'include/windows/crtl',
                    bds / 'include/dinkumware', bds / 'include/windows/rtl', bds / 'include/windows/sdk']
        libraries = [bds / 'lib/win32/release', bds / 'lib/win32/release/psdk']
        options = ['-tWM', '-tWC', '-H-', '-DWIN32', '-DUNICODE', '-D_UNICODE',
                   '-D_WIN32_WINNT=0x0601', '-I' + ';'.join(map(quoted, includes)),
                   '-L' + ';'.join(map(quoted, libraries))]
        options += ['-v', '-y', '-Od', '-D_DEBUG'] if configuration == 'Debug' else ['-O2', '-v-', '-DNDEBUG']
        options += ['-e' + quoted(executable), quoted(ROOT / 'tests/session_log_tests.cpp')]
        response = out / 'compile.rsp'
        response.write_text('\n'.join(options) + '\n', encoding='mbcs')
        built = subprocess.run([str(compiler), '@' + str(response)], cwd=out, env=env,
                               capture_output=True, text=True, errors='replace', timeout=120)
        (out / 'compile.log').write_text(built.stdout + built.stderr, encoding='utf-8')
        print(configuration + ' compile: ' + str(built.returncode))
        if built.returncode:
            print(built.stdout + built.stderr)
            return built.returncode
        run = subprocess.run([str(executable)], cwd=out, env=env, capture_output=True,
                             text=True, errors='replace', timeout=60)
        (out / 'run.log').write_text(run.stdout + run.stderr, encoding='utf-8')
        print(run.stdout + run.stderr, end='')
        record = {'configuration': configuration, 'returncode': run.returncode,
                  'hardware_io': False, 'output': run.stdout + run.stderr}
        results.append(record)
        (out / 'results.json').write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        if run.returncode:
            return run.returncode
    (ROOT / '.omx/research/2026-10-18-sync/log-tests'/('results-'+args.configuration+'.json')).write_text(
        json.dumps(results, indent=2) + '\n', encoding='utf-8')
    return 0

if __name__ == '__main__':
    sys.exit(main())
