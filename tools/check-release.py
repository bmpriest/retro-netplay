#!/usr/bin/env python3
"""Check Complete archive layout, executable modes, architecture and payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
COMPAT = ('fceumm', 'picodrive', 'snes9x', 'mednafen_supafaust', 'pcsx_rearmed', 'gambatte', 'gpsp')


def check(path, spruce):
    prefix = 'App/Netplay/' if spruce else ''
    platforms = ('tg5040', 'tg5050', 'my355', 'h700') if spruce else ('tg5040', 'tg5050', 'my282', 'my355', 'h700')
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None, 'ZIP CRC failure'
        names = set(archive.namelist())
        assert all(name.startswith(prefix) or name in ('App/',) for name in names), 'incorrect extraction layout'
        assert len(names) == len(archive.infolist()), 'duplicate ZIP entries'
        for name in names:
            assert '..' not in Path(name).parts, name
            assert not name.endswith(('.gba', '.gb', '.gbc', '.nds', '.sfc', '.nes', '.cue', '.bin')), f'unexpected ROM payload: {name}'
            assert '__pycache__' not in name and not name.endswith(('.pyc', '.log')), f'development artifact: {name}'
            assert Path(name).name not in ('session', 'settings', 'rig.conf', 'rig.json'), f'mutable fixture state: {name}'
        def require(relative, executable=False):
            name = prefix + relative
            assert name in names, f'missing {name}'
            if executable:
                assert (archive.getinfo(name).external_attr >> 16) & 0o111, f'not executable: {name}'
            return archive.read(name)
        def elf(relative, platform):
            data = require(relative, True)
            assert data[:4] == b'\x7fELF' and data[5] == 1, relative
            assert data[4] == (1 if platform == 'my282' else 2), relative
            assert struct.unpack_from('<H', data, 18)[0] == (40 if platform == 'my282' else 183), relative
            return data
        require('launch.sh', True)
        require('README.md')
        commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT).strip()
        assert require('SOURCE_COMMIT').strip() == commit, 'archive source commit differs from checkout'
        assert not any(name.startswith(prefix + 'docs/') for name in names), 'docs/ is not shipped'
        require('launcher/invite.sh', True)
        require('pak_onboarding.json')
        require('pak_hints.json')
        assert prefix + 'launcher/pak_hints.json' not in names, 'hint files left in launcher/'
        for platform in platforms:
            for name in ('netplay.elf', 'netplay-broker.elf', 'netplay-wifips.elf', 'netplay_shim.so'):
                relative = f'bin/{platform}/{name}'
                data = elf(relative, platform)
                local = ROOT/'bin'/('spruce' if spruce and name.endswith('.elf') else '')/platform/name
                assert hashlib.sha256(data).digest() == hashlib.sha256(local.read_bytes()).digest(), f'stale binary: {relative}'
            for core in ('gambatte', 'gpsp', 'gambatte_dual', 'mgba_dual'):
                elf(f'cores/override/{platform}/{core}_libretro.so', platform)
            if not spruce:
                elf(f'cores/mgba/{platform}/MGBA.pak/mgba_libretro.so', platform)
        for arch in (('aarch64',) if spruce else ('armv7', 'aarch64')):
            for core in COMPAT:
                require(f'cores/compatibility/{arch}/{core}_libretro.so')
        if spruce:
            require('spruce_ui/CHEEVOS-LICENSE')
            require('spruce_ui/footer.py')
            config = json.loads(require('config.json'))
            assert set(config['devices']) == {'TRIMUI_A133P', 'TRIMUI_SMART_PRO_S', 'MIYOO_FLIP', 'MIYOO_A30', 'MIYOO_MINI_FAMILY', 'ANBERNIC_RGXX', 'DARKMOSS'}
            for platform in platforms:
                elf(f'bin/{platform}/libdrastic_wifi.so', platform)
            # The engine behind PyUI is headless: no SDL, GL or NextUI runtime.
            engines = [n for n in names if n.startswith(prefix + 'bin/') and n.endswith('/netplay.elf')]
            assert len(engines) == 6, engines
            for name in engines:
                data = archive.read(name)
                for library in (b'libSDL2', b'libGLES', b'libEGL', b'libmsettings', b'libsamplerate', b'libzip'):
                    assert library not in data, f'{name} links {library.decode()}'
            assert not any(n.startswith(prefix + 'nextui/') for n in names), 'NextUI runtime is packaged'
            assert not any(n.startswith(prefix + 'bin/') and '/stubs/' in n for n in names), 'stub libraries are packaged'
        else:
            # DraStic comes from the Pak Store's NDS.pak; only the overlay ships.
            assert not any(name.startswith('cores/nds/base/') for name in names), 'DraStic is bundled'
            assert not any(name.startswith('Netplay.pak/') for name in names), 'NextUI archive has a wrapper folder'
            for platform in ('tg5040', 'tg5050', 'h700'):
                require(f'cores/dsp/{platform}/DSP.pak/{platform}/dsperate', True)
                require(f'cores/nds/{platform}/NDS.pak/drastic/libs/libdrastic_wifi.so')
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    print(f'{path.name}: PASS ({path.stat().st_size:,} bytes) SHA256 {digest}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', nargs='?', type=Path, default=ROOT/'dist')
    args = parser.parse_args()
    check(args.directory/'NextUI-Netplay-Complete.zip', False)
    check(args.directory/'Spruce-Netplay-Complete.zip', True)
