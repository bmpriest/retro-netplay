#!/usr/bin/env python3
"""sync.sh: the runner's desync guard and spruce prompt prevention.

The fake /proc tree stands in for a device: a RetroArch process whose
command line names the ROM it runs, one running something else, and no
game process at all. The prompt half drives a fake spruce-config.json.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SYNC = ROOT / 'testing/devbridge/device/sync.sh'


def run_sync(args, env_extra):
    env = {**os.environ, **env_extra}
    return subprocess.run(['sh', str(SYNC), *args], env=env,
                          capture_output=True, text=True)


class SyncGuard(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.proc = self.dir / 'proc'
        self.proc.mkdir()

    def tearDown(self):
        shutil.rmtree(self.dir)

    def add_process(self, pid, *argv):
        d = self.proc / str(pid)
        d.mkdir()
        (d / 'cmdline').write_bytes(b'\0'.join(a.encode() for a in argv) + b'\0')

    def test_matching_game_passes(self):
        self.add_process(42, '/mnt/SDCARD/RetroArch/ra32.a30', '-L', 'core.so',
                         '/mnt/SDCARD/Roms/GB/Tetris (World) (Rev 1).zip')
        result = run_sync(['game', 'Tetris (World) (Rev 1).zip'],
                          {'SYNC_PROC_ROOT': str(self.proc)})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_other_game_fails_and_says_what_runs(self):
        self.add_process(42, '/mnt/SDCARD/RetroArch/ra64.universal', '-L', 'core.so',
                         '/mnt/SDCARD/Roms/GBA/Mario Kart - Super Circuit (USA).zip')
        result = run_sync(['game', 'Tetris (World) (Rev 1).zip'],
                          {'SYNC_PROC_ROOT': str(self.proc)})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Mario Kart', result.stdout)

    def test_no_game_fails(self):
        self.add_process(7, '/usr/bin/python3', 'mainui.py')
        result = run_sync(['game', 'Tetris (World) (Rev 1).zip'],
                          {'SYNC_PROC_ROOT': str(self.proc)})
        self.assertNotEqual(result.returncode, 0)

    @unittest.skipUnless(shutil.which('jq'), 'jq not on host')
    def test_prompt_off_and_restore(self):
        cfg = self.dir / 'spruce-config.json'
        cfg.write_text(json.dumps({'menuOptions': {'Emulator Settings': {
            'keepSavesBetweenCores': {'selected': 'Prompt'}}}}))
        env = {'SYNC_SPRUCE_CONFIG': str(cfg), 'SYNC_STATE_DIR': str(self.dir)}
        self.assertEqual(run_sync(['prompt-off'], env).returncode, 0)
        value = json.loads(cfg.read_text())['menuOptions']['Emulator Settings']['keepSavesBetweenCores']['selected']
        self.assertEqual(value, 'Always')
        self.assertEqual(run_sync(['prompt-restore'], env).returncode, 0)
        value = json.loads(cfg.read_text())['menuOptions']['Emulator Settings']['keepSavesBetweenCores']['selected']
        self.assertEqual(value, 'Prompt')


class KeysMenuGuard(unittest.TestCase):
    """keys.sh must not refuse where the menu process stays resident
    during play (spruce on the A30 keeps PyUI's MainUI alive)."""

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.proc = self.dir / 'proc'
        self.proc.mkdir()
        self.input_log = self.dir / 'input.log'
        fake_in = self.dir / 'input'
        fake_in.write_text('#!/bin/sh\necho "$@" >> %s\n' % self.input_log)
        fake_in.chmod(0o755)
        self.env = {'KEYS_PROC_ROOT': str(self.proc), 'KEYS_IN': str(fake_in)}

    def tearDown(self):
        shutil.rmtree(self.dir)

    def add_process(self, pid, *argv):
        d = self.proc / str(pid)
        d.mkdir()
        (d / 'cmdline').write_bytes(b'\0'.join(a.encode() for a in argv) + b'\0')

    def run_keys(self):
        return subprocess.run(['sh', str(ROOT / 'testing/devbridge/device/keys.sh'),
                               'start:0.01:0'],
                              env={**os.environ, **self.env},
                              capture_output=True, text=True)

    def test_menu_only_refuses(self):
        self.add_process(9, '/usr/bin/python3', '/mnt/SDCARD/App/PyUI/main-ui/mainui.py')
        result = self.run_keys()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.input_log.exists())

    def test_game_under_resident_menu_presses(self):
        self.add_process(9, '/usr/bin/python3', '/mnt/SDCARD/App/PyUI/main-ui/mainui.py')
        self.add_process(12, '/tmp/netplay/ra/ra32.a30', '-L', 'core.so', 'game.zip')
        result = self.run_keys()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('tap start', self.input_log.read_text())


if __name__ == '__main__':
    unittest.main()
