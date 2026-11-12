"""Voice input re-focus is separate from approval-card focus; no real key events."""
import contextlib
import json
import unittest
from unittest.mock import patch,Mock
import agentpet_host as H

class FocusTests(unittest.TestCase):
    def setUp(self):
        self.stack=contextlib.ExitStack()
        for name,value in [('FOCUS_APPS',{'codex':'ChatGPT'}),('FOCUS_KEYS',{'codex':'ctrl+shift+i'}),
                           ('FOCUS_FOLLOW',True),('_focus_raised',{})]:
            self.stack.enter_context(patch.object(H,name,value))
        self.key=self.stack.enter_context(patch.object(H,'post_key'))
        self.run=self.stack.enter_context(patch.object(H.subprocess,'run'))
        self.sleep=self.stack.enter_context(patch.object(H.time,'sleep'))
        self.stack.enter_context(patch.object(H,'log'))
        self.stack.enter_context(patch.object(H,'_frontmost_app',return_value='ChatGPT'))
        self.stack.enter_context(patch.object(H,'front_agent',side_effect=lambda name:'codex' if name=='ChatGPT' else None))
    def tearDown(self):self.stack.close()
    def test_voice_reasserts_input_even_when_already_front(self):
        H.raise_agent_app('codex',focus_input=True)
        self.key.assert_called_once_with('ctrl+shift+i');self.run.assert_not_called()
    def test_approval_keeps_existing_card_focus(self):
        H.raise_agent_app('codex')
        self.key.assert_not_called();self.run.assert_not_called()
    def test_newly_raised_app_only_receives_focus_shortcut_once(self):
        with patch.object(H,'_frontmost_app',return_value='Finder'):
            H.raise_agent_app('codex',wait=0.15,focus_input=True)
        self.key.assert_called_once_with('ctrl+shift+i');self.run.assert_called_once()
        self.assertEqual([c.args[0] for c in self.sleep.call_args_list],[0.15,0.15])
    def test_non_voice_window_wait_stays_at_half_a_second(self):
        with patch.object(H,'_frontmost_app',return_value='Finder'):
            H.raise_agent_app('codex')
        self.assertEqual([c.args[0] for c in self.sleep.call_args_list],[0.5,0.15])
    def test_disabled_focus_follow_is_preserved(self):
        H.FOCUS_FOLLOW=False;H.raise_agent_app('codex',focus_input=True)
        self.key.assert_not_called();self.run.assert_not_called()
    def test_empty_shortcut_does_not_guess_keys(self):
        H.FOCUS_KEYS={};H.raise_agent_app('codex',focus_input=True);self.key.assert_not_called()
    def test_voice_handler_requests_input_focus_before_dictation(self):
        focus=Mock();voice=Mock()
        with patch.object(H,'VOICE_SOURCE','mac'),patch.object(H,'inject',side_effect=lambda fn:fn()), \
             patch.object(H,'raise_agent_app',focus),patch.object(H,'voice_key',voice),patch.object(H,'_mic_reset'):
            events=[];focus.side_effect=lambda *a,**kw:events.append('focus');voice.side_effect=lambda *a:events.append('voice')
            H.handle_board_msg(json.dumps({'t':'voice','a':'start','agent':'codex'}),lambda b:None)
        focus.assert_called_once_with('codex',wait=0.15,focus_input=True);voice.assert_called_once_with(True)
        self.assertEqual(events,['focus','voice'])
    def test_front_dictation_does_not_move_the_cursor(self):
        with patch.object(H,'VOICE_SOURCE','mac'),patch.object(H,'inject',side_effect=lambda fn:fn()), \
             patch.object(H,'raise_agent_app') as focus,patch.object(H,'voice_key'),patch.object(H,'_mic_reset'):
            H.handle_board_msg(json.dumps({'t':'voice','a':'start','agent':'codex','to':'front'}),lambda b:None)
        focus.assert_not_called()

if __name__=='__main__':unittest.main()
