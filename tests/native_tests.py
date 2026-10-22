#!/usr/bin/env python3
"""Build and run independent classic-BCC32 static engine tests; never access USB hardware."""
import argparse,json,os,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
TARGETS=('native_static_probe','native_archive_tests','native_streaming_tests','native_session_tests','native_transport_tests','native_tcp_tests','native_firmware_identity_tests','native_device_info_tests','ui_logic_tests')
def quoted(path):
    return '"'+str(path)+'"'
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configuration',choices=('Debug','Release'),default='Debug')
    parser.add_argument('--bds',type=Path,default=Path(r'C:\Program Files (x86)\Embarcadero\Studio\15.0'))
    parser.add_argument('--target',action='append',choices=TARGETS)
    parser.add_argument('--reviewed-pit',type=Path,help='Optional real reviewed package PIT for additional layout-profile coverage')
    args=parser.parse_args()
    bds=args.bds.resolve();compiler=bds/'bin/bcc32.exe'
    objects_root=ROOT/'build'/args.configuration
    expected=list((ROOT/'engine/src').glob('*.cpp'))
    objects=[objects_root/'engine'/(source.stem+'.obj') for source in sorted(expected)]
    objects += [objects_root/'lz4'/(stem+'.obj') for stem in ('lz4','lz4hc','lz4frame','xxhash')]
    missing=[str(path) for path in objects if not path.is_file()]
    if missing:raise RuntimeError('Compile native engine objects first with build.py --objects-only: '+', '.join(missing))
    destination=ROOT/'tests/native-build'/args.configuration;destination.mkdir(parents=True,exist_ok=True)
    includes=[ROOT/'include',ROOT/'engine/include',ROOT/'engine/src',ROOT/'engine/vendor/lz4',bds/'include',bds/'include/windows/crtl',bds/'include/dinkumware',bds/'include/windows/rtl',bds/'include/windows/sdk']
    libraries=[bds/'lib/win32/release',bds/'lib/win32/release/psdk']
    env=os.environ.copy();env['PATH']=str(bds/'bin')+os.pathsep+env.get('PATH','')
    results=[]
    for target in args.target or TARGETS:
        source=ROOT/'tests'/(target+'.cpp');output=destination/(target+'.exe')
        options=['-tWM','-tWC','-H-','-DWIN32','-D_WIN32_WINNT=0x0601','-DUNICODE','-D_UNICODE','-DEJ_ODIN4_CORE_STATIC','-DNOMINMAX','-I'+';'.join(quoted(p) for p in includes),'-L'+';'.join(quoted(p) for p in libraries),'-n'+quoted(destination),'-e'+quoted(output.name),quoted(source)]
        options += [quoted(path) for path in objects]+['setupapi.lib','advapi32.lib','ole32.lib','ws2_32.lib']
        response=destination/(target+'.rsp');response.write_text('\n'.join(options)+'\n',encoding='mbcs')
        started=time.time();built=subprocess.run([str(compiler),'@'+str(response)],cwd=destination,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,errors='replace')
        (destination/(target+'-build.log')).write_text(built.stdout,encoding='utf-8')
        record={'target':target,'compiler_exit':built.returncode,'static_engine_objects':len(objects),'private_dll':False}
        print(target+': compile '+str(built.returncode),flush=True)
        if built.returncode==0:
            try:
                run_args=[str(output)]
                if target=='native_streaming_tests' and args.reviewed_pit:run_args.append(str(args.reviewed_pit.resolve()))
                ran=subprocess.run(run_args,cwd=destination,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,errors='replace',timeout=180)
                record.update(exit_code=ran.returncode,output=ran.stdout)
                (destination/(target+'-run.log')).write_text(ran.stdout,encoding='utf-8');print(ran.stdout,flush=True)
            except subprocess.TimeoutExpired as error:record.update(exit_code=-1,output='Test exceeded 180-second limit')
        else:record.update(exit_code=None,output=built.stdout);print(built.stdout,flush=True)
        record['seconds']=round(time.time()-started,3);results.append(record)
    manifest={'configuration':args.configuration,'compiler':str(compiler),'hardware_io':False,'results':results}
    (destination/'results.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    return 0 if all(r['compiler_exit']==0 and r['exit_code']==0 for r in results) else 1
if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,RuntimeError) as error:print('Native tests failed: '+str(error),file=sys.stderr);sys.exit(1)
