"""Track control: the definition resource, the generated table and the reviewed GUI-tick hook."""
import copy
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tooling import native
from tooling.adapters import control_callbacks, generate_track_controls
from tooling.definitions import validate_definition
from tooling.formats import ldr, mapped_offset, sha
from tooling.pipeline import ROOT, config
from test_sample_services import HOST_CXX
from machine_fixtures import fixture


def with_control(definition, control):
    value = copy.deepcopy(definition)
    value['resources']['trackControl'] = control
    return value


class TrackControl(unittest.TestCase):
    def test_definition_declares_aligned_state(self):
        synth = fixture('probesynth').definition
        validate_definition(with_control(synth, {'stateBytes': 8, 'stateAlignment': 4}), 'synth')
        for control in ({'stateBytes': 6, 'stateAlignment': 4}, {'stateBytes': 0, 'stateAlignment': 4},
                        {'stateBytes': 8}, {'stateBytes': 8, 'stateAlignment': 4, 'tracks': 1}, 1):
            with self.subTest(control=control), self.assertRaises(ValueError):
                validate_definition(with_control(synth, control), 'synth')
        effect = fixture('probefx').definition
        with self.assertRaises(ValueError):
            validate_definition(with_control(effect, {'stateBytes': 8, 'stateAlignment': 4}), 'effect')

    def test_collection_without_track_control_links_an_empty_table(self):
        machines = [fixture(name) for name in ('probesynth', 'probefx')]
        self.assertEqual([control_callbacks(m) for m in machines], [[], []])
        self.assertNotIn('probesynth/control.o', native.object_owners(machines))
        self.assertIn('sdk/track_control.o', native.object_owners(machines))
        with tempfile.TemporaryDirectory(prefix='track-control-') as folder:
            table = generate_track_controls(folder, machines).read_text(encoding='utf-8')
        self.assertIn('{nullptr, nullptr, nullptr, nullptr}', table)
        self.assertNotIn('probesynth', table)

    def test_gui_tick_is_a_reviewed_call_in_a_shared_unit(self):
        hooks = [p for p in config()['patches'] if p.get('symbol') == 'sdk_gui_tick']
        self.assertEqual([(int(p['address'], 16), p['length'], p['call']) for p in hooks], [(0x8c0baade, 8, True)])
        self.assertNotIn('service', hooks[0])
        self.assertIn('track_control', native.SDK_UNITS)
        self.assertNotIn('track_control', native.UNIT_SERVICES)

    @unittest.skipUnless(HOST_CXX, 'No working host C++ compiler (set OPXY_TEST_HOST_CXX)')
    def test_real_platform_adapter_overlays_and_restores(self):
        platform = ROOT / 'sdk/platform/opxy-1.2.1'
        with tempfile.TemporaryDirectory(prefix='track-control-') as directory:
            folder = Path(directory)
            # Substitute only the stock-address boundary, as other SDK fixtures do.
            stock = (platform / 'stock.hpp').read_text(encoding='utf-8')
            stock = stock.replace('typedef __UINTPTR_TYPE__ Address;',
                                  'typedef __UINTPTR_TYPE__ Address;\nAddress host_stock_address(Address);')
            for pointer in ('T*', 'const volatile u32*', 'const volatile unsigned char*'):
                stock = stock.replace('reinterpret_cast<' + pointer + '>(address)',
                                      'reinterpret_cast<' + pointer + '>(host_stock_address(address))')
            (folder / 'stock.hpp').write_text(stock, encoding='utf-8')
            (folder / 'track_control.cpp').write_bytes((platform / 'track_control.cpp').read_bytes())
            binary = folder / ('test.exe' if os.name == 'nt' else 'test')
            hidden = dict(creationflags=subprocess.CREATE_NO_WINDOW) if os.name == 'nt' else {}
            result = subprocess.run([HOST_CXX, '-std=c++11', '-O2', '-Wall', '-Werror',
                '-I' + str(folder), '-I' + str(ROOT / 'sdk/include'), '-I' + str(platform),
                str(ROOT / 'tests/fixtures/track_control.cpp'), '-o', str(binary)],
                capture_output=True, text=True, timeout=120, **hidden)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30, **hidden)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(os.environ.get('OPXY_TEST_STOCK_OS'), 'Private stock input not supplied')
    def test_stock_tick_calls_the_original_leaf(self):
        from tooling.firmware import absolute_target
        stock = Path(os.environ['OPXY_TEST_STOCK_OS']).read_bytes()
        profile = config()
        self.assertEqual(sha(stock), profile['stock_sha256'])
        patch = next(p for p in profile['patches'] if p.get('symbol') == 'sdk_gui_tick')
        offset = mapped_offset(ldr(stock), int(patch['address'], 16), patch['length'])
        self.assertEqual(offset, int(patch['file_offset'], 16))
        self.assertEqual(absolute_target(stock[offset:offset + 8]), 0x8d580074)


if __name__ == '__main__':
    unittest.main()
