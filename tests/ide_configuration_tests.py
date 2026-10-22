"""Exercise C++Builder's actual Base/Debug/Release MSBuild entry points; no device I/O."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
NS = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--msbuild', type=Path, default=Path(r'C:\Windows\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe'))
    parser.add_argument('--configuration', action='append', choices=('Base', 'Debug', 'Release'))
    args = parser.parse_args()
    root = args.root.resolve()
    tree = ET.parse(root/'OdinBCC.cbproj')
    items = {item.attrib['Include']: item for item in tree.findall('.//m:BuildConfiguration', NS)}
    assert set(items) == {'Base', 'Debug', 'Release'}, 'IDE configuration registry is incomplete'
    assert items['Base'].find('m:Key', NS).text == 'Base'
    for name, key in (('Debug', 'Cfg_1'), ('Release', 'Cfg_2')):
        assert items[name].find('m:Key', NS).text == key
        assert items[name].find('m:CfgParent', NS).text == 'Base'
        assert tree.find('.//m:'+key, NS) is not None, 'IDE inheritance property missing: '+key
    assert tree.find('.//m:Base_Win32', NS) is not None
    artifacts = root/'build/ide-configuration-tests'
    artifacts.mkdir(parents=True, exist_ok=True)
    results = []
    for selection in args.configuration or ('Base', 'Debug', 'Release'):
        command = [str(args.msbuild), str(root/'OdinBCC.cbproj'), '/t:Build', '/p:Config='+selection, '/p:Platform=Win32', '/nologo']
        ran = subprocess.run(command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', timeout=180)
        (artifacts/(selection+'.log')).write_text(ran.stdout, encoding='utf-8')
        expected = 'Release' if selection == 'Release' else 'Debug'
        record = {'ide_selection': selection, 'native_configuration': expected, 'exit_code': ran.returncode}
        results.append(record)
        print(selection+' -> '+expected+': '+str(ran.returncode), flush=True)
        if ran.returncode:
            print(ran.stdout[-6000:], flush=True)
            break
        manifest = json.loads((root/'bin'/expected/'build-manifest.json').read_text())
        assert manifest['configuration'] == expected and manifest['native_engine']
        assert manifest['private_runtime_dll_required'] is False
        assert len(manifest['objects']) == 17
        assert any(Path(item['file']).name == 'Odin4FirmwareIdentity.obj' for item in manifest['objects']), 'Signed identity object is missing from IDE output'
        assert 'Native BCC configuration: '+expected+' (IDE selection: '+selection+')' in ran.stdout
    (artifacts/'results.json').write_text(json.dumps(results, indent=2)+'\n', encoding='utf-8')
    return 0 if all(item['exit_code'] == 0 for item in results) else 1

if __name__ == '__main__':
    sys.exit(main())
