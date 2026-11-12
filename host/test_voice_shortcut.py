"""Dictation shortcut configuration and press/release tests; no live input events."""
import contextlib
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import agentpet_host as H

class ShortcutTests(unittest.TestCase):
    def setUp(self):
        self.stack = contextlib.ExitStack()
        for name, value in [('VOICE_SHORTCUT','fn'),('VOICE_MODE','hold_fn'),
                            ('_voice_pressed_shortcut',None),('voice_down',False),('_HAVE_QUARTZ',True)]:
            self.stack.enter_context(patch.object(H,name,value))
        self.events=[]
        self.stack.enter_context(patch.object(H,'_post_voice_shortcut',lambda spec,down:self.events.append((spec,down))))
        self.stack.enter_context(patch.object(H,'log'))
    def tearDown(self): self.stack.close()
    def test_default_fn_press_and_release(self):
        H.voice_key(True);H.voice_key(False)
        self.assertEqual(self.events,[('fn',True),('fn',False)])
        self.assertFalse(H.voice_down)
    def test_custom_key_is_not_tapped_again_on_release(self):
        H.VOICE_SHORTCUT='option+z';H.voice_key(True);H.voice_key(False)
        self.assertEqual(self.events,[('option+z',True),('option+z',False)])
    def test_duplicates_are_ignored(self):
        for down in [False,True,True,False,False]:H.voice_key(down)
        self.assertEqual(self.events,[('fn',True),('fn',False)])
    def test_setting_change_releases_the_original_shortcut(self):
        H.VOICE_SHORTCUT='option+z';H.voice_key(True)
        H.VOICE_SHORTCUT='ctrl+space';H.voice_key(False);H.voice_key(True);H.voice_key(False)
        self.assertEqual(self.events,[('option+z',True),('option+z',False),('ctrl+space',True),('ctrl+space',False)])
    def test_invalid_runtime_setting_sends_nothing(self):
        H.VOICE_SHORTCUT='bad+z';H.voice_key(True);H.voice_key(False)
        self.assertEqual(self.events,[])
    def test_missing_quartz_does_not_latch_a_custom_key(self):
        H.VOICE_SHORTCUT='option+z';H._HAVE_QUARTZ=False;H.voice_key(True)
        self.assertFalse(H.voice_down);self.assertIsNone(H._voice_pressed_shortcut)
    def test_fn_fallback_preserved(self):
        H._HAVE_QUARTZ=False
        with patch.object(H,'_tap_fn_osascript') as tap:
            H.voice_key(True);H.voice_key(False)
            self.assertEqual([c.args[0] for c in tap.call_args_list],[2,1])
    def test_validation_and_normalization(self):
        for spec in ['fn','option+z','ctrl+shift+space','f5','cmd+f12']:
            self.assertEqual(H.voice_shortcut_clean(spec),spec)
        self.assertEqual(H.voice_shortcut_clean('  OPTION+Z '),'option+z')
        for spec in ['',None,42,'cmd','bad+z','option+alt+z','ctrl+ctrl+z','ctrl +z','f99']:
            self.assertIsNone(H.voice_shortcut_clean(spec),spec)

class KeyEventTests(unittest.TestCase):
    def test_fn_uses_original_fn_injector(self):
        with patch.object(H,'_post_fn') as fn:
            H._post_voice_shortcut('fn',True);H._post_voice_shortcut('fn',False)
            self.assertEqual([c.args[0] for c in fn.call_args_list],[True,False])
    def test_combo_emits_only_one_down_and_one_up_with_flags(self):
        from unittest.mock import Mock
        q=SimpleNamespace(kCGEventFlagMaskCommand=1,kCGEventFlagMaskShift=2,
            kCGEventFlagMaskAlternate=4,kCGEventFlagMaskControl=8,kCGHIDEventTap=0,
            CGEventCreateKeyboardEvent=Mock(side_effect=['down','up']),
            CGEventSetFlags=Mock(),CGEventPost=Mock())
        with patch.object(H,'Quartz',q),patch.object(H,'objc_pool',contextlib.nullcontext),patch.object(H,'_host_key_stamp'):
            H._post_voice_shortcut('option+z',True);H._post_voice_shortcut('option+z',False)
        self.assertEqual([c.args for c in q.CGEventCreateKeyboardEvent.call_args_list],[(None,6,True),(None,6,False)])
        self.assertEqual([c.args for c in q.CGEventSetFlags.call_args_list],[('down',4),('up',0)])

class SettingsTests(unittest.TestCase):
    def setUp(self):
        self.stack=contextlib.ExitStack();self.tmp=self.stack.enter_context(tempfile.TemporaryDirectory())
        self.config=Path(self.tmp)/'config.json'
        self.stack.enter_context(patch.object(H,'CONFIG_OVERRIDE',self.config))
        self.stack.enter_context(patch.object(H,'VOICE_SHORTCUT','fn'))
        for name in ['push_msg','np_apply','prime_voices','log']:
            self.stack.enter_context(patch.object(H,name))
    def tearDown(self):self.stack.close()
    def test_save_reload_and_preserve_other_preferences(self):
        self.config.write_text(json.dumps({'focus_apps':{'codex':'ChatGPT'},'voice_source':'board'}))
        r=H.save_settings({'voice_shortcut':' OPTION+Z '})
        self.assertTrue(r['ok']);self.assertIn('voice_shortcut',r['keys'])
        d=json.loads(self.config.read_text());self.assertEqual(d['voice_shortcut'],'option+z')
        self.assertEqual(d['focus_apps'],{'codex':'ChatGPT'});self.assertEqual(d['voice_source'],'board')
        self.assertEqual(H.VOICE_SHORTCUT,'option+z')
    def test_invalid_save_does_not_write_any_settings(self):
        self.config.write_text('{"voice_shortcut":"fn","voice_style":"chirp"}')
        before=self.config.read_bytes()
        self.assertFalse(H.save_settings({'voice_shortcut':'invalid','voice_style':'tts'})['ok'])
        self.assertEqual(self.config.read_bytes(),before)
    def test_old_config_defaults_to_fn(self):
        self.config.write_text('{}');H.load_overrides();self.assertEqual(H.VOICE_SHORTCUT,'fn')
    def test_http_save_and_read_back(self):
        from http.server import ThreadingHTTPServer
        from urllib.request import Request, urlopen
        import threading
        server=ThreadingHTTPServer(('127.0.0.1',0),H.HookHandler)
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        base='http://127.0.0.1:%d'%server.server_port
        try:
            req=Request(base+'/settings/save',data=b'{"voice_shortcut":"option+z"}',headers={'Content-Type':'application/json'})
            with urlopen(req,timeout=5) as r:self.assertTrue(json.load(r)['ok'])
            with urlopen(base+'/settings/data',timeout=5) as r:self.assertEqual(json.load(r)['voice_shortcut'],'option+z')
            with urlopen(base+'/settings',timeout=5) as r:self.assertIn('id="voice_shortcut"',r.read().decode())
        finally:
            server.shutdown();server.server_close();thread.join(2)

    def test_invalid_file_setting_falls_back_to_fn(self):
        self.config.write_text('{"voice_shortcut":"bad+z"}')
        H.load_overrides();self.assertEqual(H.VOICE_SHORTCUT,'fn')

if __name__=='__main__':unittest.main()
