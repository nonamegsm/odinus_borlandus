"""Repeatable, hardware-free native classic BCC engine and owned-window UI checks."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
TESTS=ROOT/'tests'
spec=importlib.util.spec_from_file_location('odin_bcc_build',ROOT/'build.py')
build=importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def run(argv,cwd=TESTS):
    print('Running: '+subprocess.list2cmdline([str(x) for x in argv]),flush=True)
    subprocess.run([str(x) for x in argv],cwd=cwd,check=True)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--configuration',choices=('Debug','Release'),default='Debug')
    parser.add_argument('--bds',type=Path,default=Path('C:/Program Files (x86)/Embarcadero/Studio/15.0'))
    parser.add_argument('--build-app',action='store_true',help='Build the selected UI configuration before testing')
    parser.add_argument('--firmware-dir',type=Path,help='Optional actual firmware offline plan; never opens USB')
    parser.add_argument('--reviewed-pit',type=Path,help='Optional reviewed package PIT for native layout checks')
    parser.add_argument('--native-only','--abi-only',dest='native_only',action='store_true')
    args=parser.parse_args()
    run([sys.executable,TESTS/'build_output_tests.py'])
    if args.build_app:
        run([sys.executable,ROOT/'build.py','--configuration',args.configuration,'--bds',args.bds],ROOT)
    run([sys.executable,TESTS/'run_session_log_tests.py','--configuration',args.configuration,'--bds',args.bds])
    executable=build.verified_output(args.configuration)
    native=[sys.executable,TESTS/'native_tests.py','--configuration',args.configuration,'--bds',args.bds]
    if args.reviewed_pit: native+=['--reviewed-pit',args.reviewed_pit]
    run(native)
    if not args.native_only:
        command=[sys.executable,TESTS/'ui_smoke.py',executable,
                 '--artifacts',TESTS/('ui-artifacts-'+args.configuration.lower())]
        if args.firmware_dir: command+=['--firmware-dir',args.firmware_dir]
        run(command,ROOT)
    print('PASS: no device enumeration, connection probes, PIT reads or firmware writes were requested.')


if __name__=='__main__':
    main()
