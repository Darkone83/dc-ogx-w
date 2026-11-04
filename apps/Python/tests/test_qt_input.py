"""Pure input mapping and first-run bootstrap tests; PySide6 need not be installed."""
import unittest
from ogx_input import DEFAULT_BINDINGS, check_bindings, keyboard_snapshot, rebind
from ogx_bootstrap import desktop_mode, venv_python


class QtModelTests(unittest.TestCase):
    def test_white_black_button_positions(self):
        frame = keyboard_snapshot({'Z', 'C', 'Space'}, DEFAULT_BINDINGS)
        self.assertEqual(frame['buttons'] & 0x301, 0x301)

    def test_full_neutral_frame(self):
        self.assertEqual(keyboard_snapshot(set(), DEFAULT_BINDINGS),
                         dict(buttons=0,dpad=0,lx=0,ly=0,rx=0,ry=0,lt=0,rt=0))

    def test_dpad_contradiction_is_neutral(self):
        self.assertEqual(keyboard_snapshot({'Up','Down'}, DEFAULT_BINDINGS)['dpad'], 0)

    def test_sticks_and_triggers(self):
        frame = keyboard_snapshot({'W','D','J','K','1'}, DEFAULT_BINDINGS)
        self.assertEqual((frame['lx'],frame['ly'],frame['rx'],frame['ry'],frame['lt']),
                         (32767,32767,-32767,-32767,255))

    def test_swapped_remaps(self):
        new = rebind(DEFAULT_BINDINGS,'white','C')
        self.assertEqual((new['white'],new['black']),('C','Z'))
        self.assertEqual(keyboard_snapshot({'C'},new)['buttons'],0x100)

    def test_reserved_cannot_rebind(self):
        for name in ('Escape','F8'):
            with self.assertRaises(ValueError):
                rebind(DEFAULT_BINDINGS,'a',name)

    def test_malformed_prefs_recovered(self):
        self.assertEqual(check_bindings({'white':None})['white'],'Z')
        self.assertEqual(check_bindings({'black':'Escape'})['black'],'C')

    def test_launcher_does_not_require_desktop_for_cli(self):
        self.assertFalse(desktop_mode(['status']))
        self.assertFalse(desktop_mode(['web']))
        self.assertTrue(desktop_mode([]))
        self.assertTrue(desktop_mode(['--host','192.168.1.20','desktop']))
        self.assertFalse(desktop_mode(['desktop-sdl']))

if __name__ == '__main__':
    unittest.main()
