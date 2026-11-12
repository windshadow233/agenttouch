"""Exact monotonic startup timings without recording audio or posting keys."""
import contextlib
import unittest
from unittest.mock import patch
import agentpet_host as H

class TimingTests(unittest.TestCase):
    def setUp(self):
        self.stack=contextlib.ExitStack()
        for key,val in [('_voice_startup_timing',{}),('_mic_setup_timing',{})]:
            self.stack.enter_context(patch.object(H,key,val))
        self.stack.enter_context(patch.object(H,'log'))
    def tearDown(self):self.stack.close()
    def test_parallel_phase_durations_and_join_order(self):
        events=[]
        class Future:
            def result(self):
                events.append('mic-ready')
                return True,500,{'helper_ms':1,'switch_ms':499}
        class Pool:
            def submit(self,fn):
                events.append('mic-submitted')
                return Future()
        with patch.object(H,'_voice_prepare_pool',Pool()), \
             patch.object(H.time,'perf_counter',side_effect=[10.01,10.02,10.17,10.52,10.53,10.54]), \
             patch.object(H,'voice_key',side_effect=lambda v:events.append(('key',v))), \
             patch.object(H,'mic_sink_open_gate',side_effect=lambda:events.append('gate')):
            H.voice_start_timed(lambda:events.append('focus'),True,10.0,'ble')
        self.assertEqual(events,['mic-submitted','focus','mic-ready',('key',True),'gate'])
        d=H._voice_startup_timing
        self.assertEqual([d[k] for k in ['queue_ms','mic_ms','focus_ms','key_ms','gate_ms','to_shortcut_ms','total_ms']],
                         [10,500,150,10,10,530,540])
        self.assertEqual(d['prepare_ms'],510)
        self.assertTrue(d['parallel'])
    def test_real_workers_overlap_and_keys_wait_for_both(self):
        import threading
        mic_started=threading.Event();focus_started=threading.Event();mic_done=threading.Event()
        def mic():
            mic_started.set()
            self.assertTrue(focus_started.wait(2))
            mic_done.set()
            return True
        def focus():
            focus_started.set()
            self.assertTrue(mic_started.wait(2))
        def key(start):
            self.assertTrue(mic_done.is_set());self.assertTrue(focus_started.is_set())
        with patch.object(H,'mic_sink_begin',side_effect=mic),patch.object(H,'voice_key',side_effect=key),patch.object(H,'mic_sink_open_gate'):
            H.voice_start_timed(focus,True,H.time.perf_counter())
    def test_focus_failure_joins_and_restores_route(self):
        with patch.object(H,'mic_sink_begin',return_value=True),patch.object(H,'mic_sink_end') as end,patch.object(H,'voice_key') as key:
            with self.assertRaises(RuntimeError):
                H.voice_start_timed(lambda:(_ for _ in ()).throw(RuntimeError('focus failed')),True,H.time.perf_counter())
        end.assert_called_once();key.assert_not_called()
    def test_prepare_exception_falls_back_to_mac_mic(self):
        with patch.object(H,'mic_sink_begin',side_effect=RuntimeError('route failed')),patch.object(H,'voice_key') as key,patch.object(H,'mic_sink_open_gate') as gate:
            H.voice_start_timed(lambda:None,True,H.time.perf_counter())
        key.assert_called_once_with(True);gate.assert_not_called()
        self.assertFalse(H._voice_startup_timing['route_ok'])
    def test_mac_mic_does_not_switch_or_open_board_route(self):
        with patch.object(H,'mic_sink_begin') as mic,patch.object(H,'voice_key'),patch.object(H,'mic_sink_open_gate') as gate:
            H.voice_start_timed(lambda:None,False,H.time.perf_counter())
        mic.assert_not_called();gate.assert_not_called()
        self.assertEqual(H._voice_startup_timing['source'],'mac')
        self.assertNotIn('mic_switch_ms',H._voice_startup_timing)
    def test_failed_route_is_reported_and_does_not_open_gate(self):
        with patch.object(H,'mic_sink_begin',return_value=False),patch.object(H,'voice_key') as key,patch.object(H,'mic_sink_open_gate') as gate:
            H.voice_start_timed(lambda:None,True,H.time.perf_counter())
        gate.assert_not_called();key.assert_called_once_with(True)
        self.assertFalse(H._voice_startup_timing['route_ok'])
    def test_helper_and_device_switch_are_measured_separately(self):
        with patch.object(H.time,'perf_counter',side_effect=[1,1.01,2.21]), \
             patch.object(H,'mic_sink_proc',return_value=object()), \
             patch.object(H,'mic_input_set',return_value=H.MIC_SINK_DEVICE):
            self.assertTrue(H.mic_sink_begin())
        self.assertEqual(H._mic_setup_timing,{'helper_ms':10,'switch_ms':1200})
    def test_missing_helper_skips_device_switch(self):
        with patch.object(H,'mic_sink_proc',return_value=None),patch.object(H,'mic_input_set') as switch:
            self.assertFalse(H.mic_sink_begin())
        switch.assert_not_called()
    def test_snapshot_exposes_copy_not_shared_mutable_metrics(self):
        H._voice_startup_timing.update(total_ms=123)
        d=H.mic_snapshot();d['startup']['total_ms']=999
        self.assertEqual(H._voice_startup_timing['total_ms'],123)

class ReadyPromptTests(unittest.TestCase):
    def setUp(self):
        sleeper=patch.object(H.time,'sleep')
        self.sleep=sleeper.start()
        self.addCleanup(sleeper.stop)

    def test_prompt_is_dispatched_only_after_successful_shortcut(self):
        for success in (True, None):
            with self.subTest(success=success), \
                 patch.object(H,'voice_key',return_value=success), \
                 patch.object(H.threading,'Thread') as thread, \
                 patch.object(H,'log'):
                H.voice_start_timed(lambda:None,False,H.time.perf_counter())
                if success:
                    thread.assert_called_once_with(target=H.voice_ready_prompt,daemon=True)
                    thread.return_value.start.assert_called_once()
                else:
                    thread.assert_not_called()

    def test_prompt_plays_on_board_independent_of_voice_style(self):
        with patch.object(H,'boards',{'board':object()}), \
             patch.object(H,'voice_down',True),patch.object(H,'LANG','zh'), \
             patch.object(H,'VOICE_STYLE','chirp'), \
             patch.object(H,'tts_wav',return_value='ready.wav') as tts, \
             patch.object(H,'speak_push') as push:
            H.voice_ready_prompt()
        tts.assert_called_once_with('请讲');push.assert_called_once_with('ready.wav')
        self.sleep.assert_called_once_with(1.5)

    def test_release_or_repress_during_delay_cancels_prompt(self):
        for repress in (False,True):
            def release(seconds):
                H.voice_down=repress
                H._voice_press_generation+=1
            with self.subTest(repress=repress), \
                 patch.object(H,'boards',{'board':object()}), \
                 patch.object(H,'voice_down',True), \
                 patch.object(H,'_voice_press_generation',1), \
                 patch.object(H,'tts_wav') as tts:
                self.sleep.side_effect=release
                H.voice_ready_prompt()
                tts.assert_not_called()

    def test_release_during_synthesis_cancels_prompt(self):
        def synth(text):
            H.voice_down=False
            return 'ready.wav'
        with patch.object(H,'boards',{'board':object()}), \
             patch.object(H,'voice_down',True), \
             patch.object(H,'tts_wav',side_effect=synth),patch.object(H,'speak_push') as push:
            H.voice_ready_prompt()
        push.assert_not_called()

    def test_no_prompt_after_release_or_without_tcp(self):
        for down,boards in [(False,{'board':object()}),(True,{})]:
            with patch.object(H,'voice_down',down),patch.object(H,'boards',boards), \
                 patch.object(H,'tts_wav') as tts:
                H.voice_ready_prompt()
            tts.assert_not_called()

if __name__=='__main__':unittest.main()
