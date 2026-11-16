"""Ready speech follows a confirmed system start, never just a board press."""
import contextlib
import unittest
from unittest.mock import Mock, patch

import agentpet_host as H


class DictationReadyTests(unittest.TestCase):
    def setUp(self):
        self.stack = contextlib.ExitStack()
        self.stack.enter_context(patch.object(H, '_dictation', {
            'listening': None, 'observer': True, 'request': 0,
            'held': False, 'pending_until': 0.0}))
        self.stack.enter_context(patch.object(H, 'log'))
        self.thread = self.stack.enter_context(patch.object(H.threading, 'Thread'))

    def tearDown(self):
        self.stack.close()

    def press(self):
        request = H.voice_ready_request(True)
        H.voice_ready_arm(request)
        return request

    def test_start_confirms_once_and_stop_clears_state(self):
        request = self.press()
        self.thread.assert_not_called()
        H.dictation_notification('DictationIMNotificationWillStartListening')
        self.thread.assert_not_called()
        H.dictation_notification(H.DICTATION_START)
        H.dictation_notification(H.DICTATION_START)
        self.thread.assert_called_once_with(
            target=H._voice_ready_speak, args=(request,), daemon=True)
        H.dictation_notification(H.DICTATION_STOP)
        self.assertFalse(H._dictation['listening'])

    def test_keyboard_dictation_without_board_press_is_silent(self):
        H.dictation_notification(H.DICTATION_START)
        self.assertTrue(H._dictation['listening'])
        self.thread.assert_not_called()

    def test_release_cancels_even_before_queued_key_injection(self):
        request = H.voice_ready_request(True)
        H.voice_ready_request(False)
        H.voice_ready_arm(request)
        H.dictation_notification(H.DICTATION_START)
        self.thread.assert_not_called()

    def test_unconfirmed_start_expires(self):
        with patch.object(H.time, 'monotonic', return_value=10):
            self.press()
        with patch.object(H.time, 'monotonic', return_value=19):
            H.dictation_notification(H.DICTATION_START)
        self.thread.assert_not_called()

    def test_old_cancellation_does_not_cancel_new_press(self):
        old = self.press()
        new = self.press()
        H.voice_ready_cancel(old)
        H.dictation_notification(H.DICTATION_START)
        self.assertEqual(self.thread.call_args.kwargs['args'], (new,))

    def test_release_during_synthesis_suppresses_stale_audio(self):
        request = self.press()
        H.dictation_notification(H.DICTATION_START)
        def synth(*args):
            H.voice_ready_request(False)
            return '/cached/ready.wav'
        with patch.object(H, 'tts_wav', side_effect=synth), \
                patch.object(H, 'speak_push') as speak, patch.object(H, 'boards', [Mock()]):
            H._voice_ready_speak(request)
        speak.assert_not_called()

    def test_confirmed_prompt_uses_chinese_voice_and_existing_board_audio(self):
        request = self.press()
        H.dictation_notification(H.DICTATION_START)
        with patch.object(H, 'tts_wav', return_value='/cached/ready.wav') as synth, \
                patch.object(H, 'speak_push') as speak, patch.object(H, 'boards', [Mock()]):
            H._voice_ready_speak(request)
        synth.assert_called_once_with('主人，请讲', H.TTS_VOICES['zh'])
        speak.assert_called_once_with('/cached/ready.wav')

    def test_failed_shortcut_cancels_prompt(self):
        request = H.voice_ready_request(True)
        with patch.object(H, 'voice_key', return_value=None):
            H.voice_start_timed(lambda: None, False, H.time.perf_counter(),
                                prompt_request=request)
        H.dictation_notification(H.DICTATION_START)
        self.thread.assert_not_called()


if __name__ == '__main__':
    unittest.main()
