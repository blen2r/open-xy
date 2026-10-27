"""Track control: the definition resource, the generated table and the reviewed GUI-tick hook."""
import copy
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tooling import native
from tooling.adapters import control_callbacks, generate_control, generate_track_controls
from tooling.definitions import hidden_pages, validate_definition
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
        validate_definition(with_control(synth, {'stateBytes': 8, 'stateAlignment': 4, 'savedBytes': 8192}), 'synth')
        for control in ({'stateBytes': 6, 'stateAlignment': 4}, {'stateBytes': 0, 'stateAlignment': 4},
                        {'stateBytes': 8}, {'stateBytes': 8, 'stateAlignment': 4, 'tracks': 1}, 1,
                        {'stateBytes': 8, 'stateAlignment': 4, 'savedBytes': 0},
                        {'stateBytes': 8, 'stateAlignment': 4, 'savedBytes': 8193}):
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
        self.assertIn('{nullptr, nullptr, nullptr, nullptr, nullptr, 0, nullptr, nullptr}', table)
        self.assertNotIn('probesynth', table)

    def test_a_machine_that_keeps_data_exports_save_and_load(self):
        synth = copy.deepcopy(fixture('probesynth'))
        synth.definition['resources']['trackControl'] = {'stateBytes': 8, 'stateAlignment': 4, 'savedBytes': 100}
        self.assertEqual(control_callbacks(synth)[2:], [synth.prefix + '_control_save', synth.prefix + '_control_load'])
        with tempfile.TemporaryDirectory(prefix='track-control-') as folder:
            table = generate_track_controls(folder, [synth]).read_text(encoding='utf-8')
            binding = generate_control(folder, synth).read_text(encoding='utf-8')
        p = synth.prefix
        self.assertIn(f'"{synth.id}", 100, {p}_control_save, {p}_control_load}}', table)
        self.assertIn('control_save(state, data, 100)', binding)
        self.assertIn('return bytes <= 100 ? bytes : 0;', binding)

    def test_gui_tick_is_a_reviewed_call_in_a_shared_unit(self):
        hooks = [p for p in config()['patches']
                 if p.get('symbol') in ('sdk_gui_tick', 'sdk_param_edit_set', 'sdk_param_shown_hook')]
        self.assertEqual([(int(p['address'], 16), p['length'], p['call']) for p in hooks],
                         [(0x8c0baade, 8, True), (0x8d5ad5ae, 4, True), (0x8d5aca48, 4, False)])
        self.assertTrue(all('service' not in p for p in hooks))
        # CC = R0 == 0; IF CC JUMP 8d5aca80: the getter's choice of live or base value.
        self.assertEqual(hooks[2]['before'], '000c1b18')
        for unit in ('track_control', 'track_control_hooks'):
            self.assertIn(unit, native.SDK_UNITS)
            self.assertNotIn(unit, native.UNIT_SERVICES)

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

    @unittest.skipUnless(HOST_CXX, 'No working host C++ compiler (set OPXY_TEST_HOST_CXX)')
    def test_real_hidden_pages_check(self):
        platform = ROOT / 'sdk/platform/opxy-1.2.1'
        with tempfile.TemporaryDirectory(prefix='synth-pages-') as directory:
            folder = Path(directory)
            stock = (platform / 'stock.hpp').read_text(encoding='utf-8')
            stock = stock.replace('typedef __UINTPTR_TYPE__ Address;',
                                  'typedef __UINTPTR_TYPE__ Address;\nAddress host_stock_address(Address);')
            for pointer in ('T*', 'const volatile u32*', 'const volatile unsigned char*'):
                stock = stock.replace('reinterpret_cast<' + pointer + '>(address)',
                                      'reinterpret_cast<' + pointer + '>(host_stock_address(address))')
            (folder / 'stock.hpp').write_text(stock, encoding='utf-8')
            (folder / 'synth_controls.cpp').write_bytes((platform / 'synth_controls.cpp').read_bytes())
            binary = folder / ('test.exe' if os.name == 'nt' else 'test')
            hidden = dict(creationflags=subprocess.CREATE_NO_WINDOW) if os.name == 'nt' else {}
            result = subprocess.run([HOST_CXX, '-std=c++11', '-O2', '-Wall', '-Werror',
                '-I' + str(folder), '-I' + str(ROOT / 'sdk/include'), '-I' + str(platform),
                str(ROOT / 'tests/fixtures/synth_pages.cpp'), '-o', str(binary)],
                capture_output=True, text=True, timeout=120, **hidden)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30, **hidden)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_synth_can_hide_the_envelope_and_filter_pages(self):
        synth = copy.deepcopy(fixture('probesynth').definition)
        for pages in (['envelope'], ['filter', 'envelope']):
            validate_definition(dict(synth, hidePages=pages), 'synth')
        for pages in ([], ['lfo'], ['filter', 'filter'], 'filter'):
            with self.subTest(pages=pages), self.assertRaises(ValueError):
                validate_definition(dict(synth, hidePages=pages), 'synth')
        self.assertEqual(hidden_pages(dict(synth, hidePages=['envelope', 'filter'])), 6)

    @unittest.skipUnless(os.environ.get('OPXY_TEST_STOCK_OS'), 'Private stock input not supplied')
    def test_hooked_calls_reach_the_original_functions(self):
        from tooling.firmware import absolute_target, decode_branch
        stock = Path(os.environ['OPXY_TEST_STOCK_OS']).read_bytes()
        profile = config()
        self.assertEqual(sha(stock), profile['stock_sha256'])
        patch = next(p for p in profile['patches'] if p.get('symbol') == 'sdk_gui_tick')
        offset = mapped_offset(ldr(stock), int(patch['address'], 16), patch['length'])
        self.assertEqual(offset, int(patch['file_offset'], 16))
        self.assertEqual(absolute_target(stock[offset:offset + 8]), 0x8d580074)
        patch = next(p for p in profile['patches'] if p.get('symbol') == 'sdk_param_edit_set')
        address = int(patch['address'], 16)
        offset = mapped_offset(ldr(stock), address, patch['length'])
        self.assertEqual(offset, int(patch['file_offset'], 16))
        self.assertEqual(decode_branch(address, stock[offset:offset + 4]), (0x8d5acf28, True))
        patch = next(p for p in profile['patches'] if p.get('symbol') == 'sdk_param_shown_hook')
        offset = mapped_offset(ldr(stock), int(patch['address'], 16), patch['length'])
        self.assertEqual(offset, int(patch['file_offset'], 16))
        self.assertEqual(stock[offset:offset + 4].hex(), patch['before'])


if __name__ == '__main__':
    unittest.main()
