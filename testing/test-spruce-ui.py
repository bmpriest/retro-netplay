#!/usr/bin/env python3
"""Exercise spruce menu semantics without loading SDL or touching a device."""
import os
from enum import Enum
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
os.environ.setdefault('PLATFORM', 'tg5040')
from app.spruce import __main__ as ui, footer
from app.spruce.buttons import Button

class Input(Enum):
    A='A'; B='B'; X='X'; Y='Y'

class Entry:
    def __init__(self, label, **kwargs):self.value=kwargs['value']

class Selection:
    def __init__(self, entry, pressed):self.entry=entry;self.pressed=pressed
    def get_input(self):return self.pressed
    def get_selection(self):return self.entry

class MenuTests(unittest.TestCase):
    def menu(self, rows, presses, **kwargs):
        observed=[]
        class View:
            selected=0
            def get_selection(self, accepted):
                observed.append(ui.FOOTER_CONTEXT())
                key=presses.pop(0)
                if key=='DOWN':self.selected+=1;key=None
                if key is not None:key=Input[key]
                return Selection(Entry('',value=rows[self.selected][0]),key)
            def view_finished(self):pass
        modules={
            'controller.controller_inputs':types.SimpleNamespace(ControllerInput=Input),
            'views.grid_or_list_entry':types.SimpleNamespace(GridOrListEntry=Entry),
            'views.view_creator':types.SimpleNamespace(ViewCreator=types.SimpleNamespace(create_view=lambda *a,**k:View())),
            'views.view_type':types.SimpleNamespace(ViewType=types.SimpleNamespace(ICON_AND_DESC=0)),
        }
        with patch.dict(sys.modules,modules):
            return ui.menu('test', rows, **kwargs),observed

    def test_readonly_a_ignored_and_hints_follow_selection(self):
        rows=[('FC','FC','FCEUmm','✓'),('nickname','Nickname','','')]
        result,observed=self.menu(rows,['A','DOWN','A'],actions={'FC':None,'nickname':'CHANGE'})
        self.assertEqual(result,('nickname','A'))
        self.assertEqual(observed[0],[(Button.B,'BACK')])
        self.assertEqual(observed[-1],[(Button.B,'BACK'),(Button.A,'CHANGE')])

    def test_main_back_says_exit(self):
        _,observed=self.menu([('config','Configuration','','')],['B'],actions={},back='EXIT')
        self.assertEqual(observed[0][0],(Button.B,'EXIT'))

    def test_configuration_row_sits_above_bottom_bar(self):
        drawn = []
        device = types.SimpleNamespace(screen_width=lambda: 640, screen_height=lambda: 480)
        display = types.SimpleNamespace(
            bottom_bar=types.SimpleNamespace(get_bottom_bar_height=lambda: 40),
            get_top_bar_height=lambda **_: 40,
            render_image=lambda image, x, y, **kwargs: drawn.append((image, y, kwargs['target_height'])),
            render_text=lambda label, x, y, *_: drawn.append((label, y)),
        )
        theme = types.SimpleNamespace(get_descriptive_list_icon_offset_x=lambda: 10,
                                      get_descriptive_list_text_from_icon_offset=lambda: 4,
                                      text_color_selected=lambda _: 0)
        view = types.SimpleNamespace(
            each_entry_height=60, options=[0, 1, 2], selected=2, selected_bg='selected',
            center_selection=lambda: None, row_at_touch_point=lambda _: 2,
        )
        view._render = lambda: drawn.append(('body', view.current_bottom))
        modules = {
            'devices.device': types.SimpleNamespace(Device=types.SimpleNamespace(get_device=lambda: device)),
            'display.display': types.SimpleNamespace(Display=display),
            'display.font_purpose': types.SimpleNamespace(FontPurpose=types.SimpleNamespace(DESCRIPTIVE_LIST_TITLE=0)),
            'display.render_mode': types.SimpleNamespace(RenderMode=types.SimpleNamespace(MIDDLE_LEFT_ALIGNED=0)),
            'display.resize_type': types.SimpleNamespace(ResizeType=types.SimpleNamespace(ZOOM=0)),
            'themes.theme': types.SimpleNamespace(Theme=theme),
        }
        with patch.dict(sys.modules, modules):
            ui.pin_last_row(view, 2, 'Configuration')
            view._render()
        self.assertEqual(drawn, [('body', 2), ('selected', 380, 60), ('Configuration', 410)])
        self.assertEqual(view.row_at_touch_point((20, 410)), 2)
        self.assertIsNone(view.row_at_touch_point((20, 180)))

    def test_active_x_ends_and_unwinds(self):
        with patch.object(ui,'session_active',return_value=True),patch.object(ui,'attempt',return_value=True) as action:
            with self.assertRaises(ui.SessionEnded):self.menu([('FC','FC','','')],['X'])
            action.assert_called_once_with('Netplay','Ending session','end')
        self.assertIsNone(ui.FOOTER_CONTEXT)

    def test_inactive_x_does_nothing(self):
        with patch.object(ui,'session_active',return_value=False),patch.object(ui,'attempt') as action:
            result,_=self.menu([('FC','FC','','')],['X','B'])
            self.assertIsNone(result);action.assert_not_called()

    def test_core_accordion_groups_and_available_engines(self):
        output = "\n".join((
            "core_engine\tGB\tgambatte\tGambatte\tdual\t1",
            "core_engine\tGB\tarmgba\tarmGBA\tabsent\t0",
            "core_engine\tGBA\tmgba\tmGBA\tpaired\t1",
            "core_engine\tGG\tpicodrive\tPicoDrive\tunsupported\t0",
            "core_engine\tNDS\tdsperate\tDSperate\tpresent\t0",
            "core_engine\tNDS\tdrastic\tDraStic\tready\t0",
            "core_engine\tSFC\tmednafen_supafaust\tSupafaust\tready\t0",
        ))
        rows, actions = ui.core_rows(output)
        self.assertEqual([row[0] for row in rows],
                         ['link', 'GB', 'GBA', 'GG', 'NDS', 'shared', 'FC', 'SFC', 'MD', 'SEGACD', 'SMS', 'PS'])
        self.assertFalse(any('/' in row[0] for row in rows))
        rows, actions = ui.core_rows(output, {'GB', 'GG', 'NDS', 'SFC'})
        entries = {row[0]: row for row in rows}
        self.assertNotIn('GB/armgba', entries)
        self.assertNotIn('GB/gambatte', entries)
        self.assertEqual(entries['GB'][1:4], ('GB/GBC (Gambatte)', '', '✓  >'))
        self.assertEqual(entries['GG'][3], '○')
        self.assertEqual(entries['NDS/dsperate'][3], '○')
        self.assertEqual(entries['NDS/drastic'][3], '✓')
        self.assertEqual(entries['SFC'][1], 'SNES / SFC (Supafaust)')
        self.assertEqual(entries['NDS'][3], '▾')
        self.assertNotIn('✓', entries['NDS'][3])
        self.assertEqual(actions['GB'], 'SELECT')
        self.assertEqual(actions['GBA'], 'SELECT')
        self.assertIsNone(actions['GG'])

    def test_gb_core_options_cycle_with_a_left_and_right(self):
        # Netplay decides Gambatte Dual and armGBA's Link Mode; the player
        # keeps armGBA's link settings.
        selections = iter([('< 2 >', ('armgba_link_players', 'A')),
                           ('< 3 >', ('armgba_link_players', 'DPAD_LEFT')),
                           ('< 2 >', ('armgba_link_players', 'DPAD_LEFT')),
                           ('< 4 >', None)])
        def pick(title, rows, **kwargs):
            value, selection = next(selections)
            self.assertEqual(title, 'GB/GBC')
            self.assertEqual(kwargs['extra'], ('DPAD_LEFT', 'DPAD_RIGHT'))
            labels = [row[1] for row in rows]
            self.assertNotIn('  Gambatte Dual', labels)
            self.assertNotIn('  Link Mode', labels)
            self.assertEqual(rows[0][1], '  Number of Players')
            self.assertEqual(rows[0][3], value)
            return selection
        with patch.object(ui, 'menu', side_effect=pick), patch.object(ui, 'control') as control:
            self.assertTrue(ui.core_options('GB', 'GB/GBC', 'armgba_link_players=2'))
        self.assertEqual([call.args for call in control.call_args_list],
                         [('core-option', 'armgba_link_players', '3'),
                          ('core-option', 'armgba_link_players', '2'),
                          ('core-option', 'armgba_link_players', '4')])

    def test_gpsp_cable_mode_exposes_supported_games(self):
        selections = iter([('gpsp_mode', 'A'), ('gpsp_mode', 'A'), ('gpsp_serial', 'A'), None])
        def pick(title, rows, **kwargs):
            option = next(selections)
            if option and option[0] == 'gpsp_serial':
                self.assertIn(('gpsp_serial', '  Cable Game', '', '< Pokémon Gen III >'), rows)
            return option
        with patch.object(ui, 'menu', side_effect=pick), patch.object(ui, 'control') as control:
            self.assertTrue(ui.core_options('GBA', 'gpSP', '', engine='gpsp'))
        self.assertEqual([call.args for call in control.call_args_list],
                         [('core-option', 'gpsp_serial', 'rfu'),
                          ('core-option', 'gpsp_serial', 'mul_poke'),
                          ('core-option', 'gpsp_serial', 'mul_aw1')])

    def test_selecting_one_core_exposes_only_its_settings(self):
        def pick(title, rows, **kwargs):
            self.assertEqual(title, 'armGBA')
            self.assertEqual([row[0] for row in rows], ['armgba_link_players', 'armgba_link_port'])
            return None
        with patch.object(ui, 'menu', side_effect=pick):
            self.assertFalse(ui.core_options('GBA', 'armGBA', '', engine='armgba'))

    def test_a_core_netplay_configures_itself_has_no_settings(self):
        # mGBA Dual is Netplay's call (app/engines.c): no menu at all.
        with patch.object(ui, 'menu') as menu:
            self.assertFalse(ui.core_options('GBA', 'mGBA', 'mgba_dual=1', engine='mgba'))
        menu.assert_not_called()

    def test_accordion_scroll_and_touch_follow_compact_row_heights(self):
        from app.spruce.core_list import style_rows
        output = "\n".join(f"core_engine\t{tag}\ttest{number}\tTest emulator\tready\t0"
                           for tag, _ in ui.CORE_GROUPS for number in (1, 2))
        rows, _ = ui.core_rows(output, {tag for tag, _ in ui.CORE_GROUPS})
        compact = frozenset(key for key, *_ in rows if '/' in key)
        for screen_height in (480, 720):
            with self.subTest(screen_height=screen_height):
                drawn = []
                class Font(Enum):
                    DESCRIPTIVE_LIST_TITLE = 24
                    DESCRIPTIVE_LIST_DESCRIPTION = 16
                display = types.SimpleNamespace(
                    bottom_bar=types.SimpleNamespace(get_bottom_bar_height=lambda: 40),
                    get_top_bar_height=lambda **_: 40,
                    get_text_dimensions=lambda purpose, text: (len(text) * 10, purpose.value),
                    render_image=lambda *args, **kwargs: None,
                    render_text=lambda label, x, y, color, purpose, mode, **kwargs:
                        drawn.append((label, y, purpose)),
                )
                theme = types.SimpleNamespace(
                    get_descriptive_list_icon_offset_x=lambda: 10,
                    get_descriptive_list_text_from_icon_offset=lambda: 4,
                    text_color_selected=lambda _: (240, 230, 210), text_color=lambda _: (240, 230, 210),
                )
                device = types.SimpleNamespace(screen_width=lambda: 640, screen_height=lambda: screen_height)
                modules = {
                    'devices.device': types.SimpleNamespace(Device=types.SimpleNamespace(get_device=lambda: device)),
                    'display.display': types.SimpleNamespace(Display=display),
                    'display.font_purpose': types.SimpleNamespace(FontPurpose=Font),
                    'display.render_mode': types.SimpleNamespace(RenderMode=types.SimpleNamespace(MIDDLE_LEFT_ALIGNED=0)),
                    'display.resize_type': types.SimpleNamespace(ResizeType=types.SimpleNamespace(ZOOM=0)),
                    'themes.theme': types.SimpleNamespace(Theme=theme),
                }
                view = types.SimpleNamespace(each_entry_height=64, current_top=0,
                                             selected=len(rows)-1, selected_bg='selected')
                with patch.dict(sys.modules, modules):
                    style_rows(view, rows, ui.CORE_HEADINGS, compact)
                    view._render()
                    self.assertEqual(view.current_bottom, len(rows))
                    self.assertGreater(view.current_top, 0)
                    for label, y, purpose in drawn:
                        self.assertGreaterEqual(y, 45)
                        self.assertLess(y, screen_height - 40)
                        hit = view.row_at_touch_point((20, y))
                        self.assertIsNotNone(hit)
                    engine = next(item for item in drawn if item[0] == 'Test emulator')
                    self.assertEqual(engine[2], Font.DESCRIPTIVE_LIST_DESCRIPTION)
                    view.selected = 1
                    view.adjust_selected_top_bottom_for_overflow()
                    self.assertEqual(view.current_top, 0)
                    drawn.clear()
                    view._render()
                    self.assertEqual(drawn[0][0], 'Link Play')
                    self.assertIsNone(view.row_at_touch_point((20, screen_height - 10)))

    def test_single_core_system_opens_its_settings_directly(self):
        output = 'core_engine\tGB\tgambatte\tGambatte\tdual\t1'
        ready = types.SimpleNamespace(is_set=lambda: True)
        with patch.object(ui, 'CORE_READY', ready), \
             patch.object(ui, 'CORE_RESULTS', {'all': (True, output)}), \
             patch.object(ui, 'menu', side_effect=[('GB', 'A'), None]), \
             patch.object(ui, 'core_options', return_value=False) as options:
            ui.cores()
        options.assert_called_once_with('GB', 'GB/GBC - Gambatte', output, engine='gambatte')

    def test_hint_order(self):
        self.assertEqual(footer.hints('A SELECT    B BACK'),[(Button.B,'BACK'),(Button.A,'SELECT')])

    def test_active_footer_keeps_end_session_clear_of_index_on_small_screen(self):
        drawn=[]
        class Font(Enum):
            DESCRIPTIVE_LIST_TITLE=16
            DESCRIPTIVE_LIST_DESCRIPTION=12
        display=types.SimpleNamespace(
            bottom_bar=types.SimpleNamespace(get_bottom_bar_height=lambda:40),
            get_image_dimensions=lambda path:(26,26),
            get_text_dimensions=lambda purpose,label:(purpose.value*len(label),26),
            render_image=lambda *args:None,
            render_text=lambda label,x,y,color,purpose,mode:drawn.append((label,x,purpose.value*len(label))))
        modules={
            'devices.device':types.SimpleNamespace(Device=types.SimpleNamespace(get_device=lambda:types.SimpleNamespace(screen_width=lambda:640,screen_height=lambda:480))),
            'display.display':types.SimpleNamespace(Display=display),
            'display.font_purpose':types.SimpleNamespace(FontPurpose=Font),
            'display.render_mode':types.SimpleNamespace(RenderMode=types.SimpleNamespace(MIDDLE_LEFT_ALIGNED=0)),
            'themes.theme':types.SimpleNamespace(Theme=types.SimpleNamespace(text_color=lambda purpose:0)),
        }
        with patch.dict(sys.modules,modules),patch.object(footer.glyphs,'glyph',return_value=Path('badge.png')):
            footer.render([(Button.B,'BACK'),(Button.A,'CHANGE')],True)
        self.assertEqual([row[0] for row in drawn],['BACK','CHANGE','END SESSION'])
        for first,second in zip(drawn,drawn[1:]):self.assertLessEqual(first[1]+first[2],second[1]-26)
        self.assertLessEqual(drawn[-1][1]+drawn[-1][2],640-12*len('000/0000'))

if __name__=='__main__':unittest.main()
