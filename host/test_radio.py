"""Mac only configures board RSS; no audio download/transcoding is started here."""
import json
from pathlib import Path
import contextlib
import unittest
from unittest.mock import patch
import agentpet_host as H

class RadioTests(unittest.TestCase):
 def setUp(self):
  self.stack=contextlib.ExitStack()
  self.stack.enter_context(patch.object(H,'board_radio_supported',True))
  self.stack.enter_context(patch.object(H,'_ble_client',object()))
  self.sent=[];self.stack.enter_context(patch.object(H,'push_board',self.sent.append))
  self.reply=self.stack.enter_context(patch.object(H,'wait_reply',return_value={'ok':True,'supported':True}))
 def tearDown(self):self.stack.close()
 def test_config_is_a_single_board_command(self):
  self.assertTrue(H.radio_request('config','https://feeds.daopub.com/ld.xml')['ok'])
  self.assertEqual(len(self.sent),1);self.assertEqual(self.sent[0]['op'],'config')
  self.assertTrue(self.sent[0]['enabled']);self.assertEqual(self.sent[0]['feed'],'https://feeds.daopub.com/ld.xml')
 def test_save_can_preserve_sd_mode(self):
  self.assertTrue(H.radio_request('config','https://example.test/rss',False)['ok'])
  self.assertFalse(self.sent[0]['enabled'])
 def test_no_config_on_unsupported_firmware(self):
  H.board_radio_supported=False
  self.assertFalse(H.radio_request('config','https://example.test/rss')['ok']);self.assertEqual(self.sent,[])
 def test_invalid_urls_never_reach_board(self):
  for url in ['file:///etc/passwd','ftp://example.test/feed','https://user:pass@example.test/rss','https://example.test/\nBad',None,'x'*2000]:
   self.assertFalse(H.radio_request('config',url)['ok'])
  self.assertEqual(self.sent,[])
 def test_status_and_disable_do_not_change_feed(self):
  H.radio_request('status');H.radio_request('disable')
  self.assertEqual([x['op'] for x in self.sent],['status','disable'])
  self.assertTrue(all('feed' not in x for x in self.sent))
 def test_missing_link_is_reported(self):
  H._ble_client=None
  with patch.object(H,'au_has_tcp',return_value=False):self.assertFalse(H.radio_request('config','https://example.test/rss')['ok'])
  self.assertEqual(self.sent,[])
 def test_refresh_only_requests_board_update(self):
  self.assertTrue(H.radio_request('refresh')['ok'])
  self.assertEqual(self.sent[0]['op'],'refresh')
  self.assertNotIn('feed',self.sent[0])
 def test_local_update_copies_metadata_then_imports(self):
  rss=b'<rss><channel><title>Show</title><item><title>Episode</title><enclosure url="https://example.test/one.mp3"/></item></channel></rss>'
  copied=[]
  def copy(src,dst,progress=None,progress_bytes=None):
   copied.append((json.loads(Path(src).read_text()),dst))
   if progress:progress(Path(src).stat().st_size,Path(src).stat().st_size)
   return {'ok':True}
  with patch.object(H,'au_has_tcp',return_value=True), patch.object(H,'radio_request',side_effect=[{'ok':True,'enabled':True,'feed':'https://example.test/rss'},{'ok':True}]) as request, patch.object(H,'au_get',return_value=(rss,'text/xml')), patch.object(H,'push_file',side_effect=copy):
   self.assertTrue(H.radio_update_local()['ok'])
   self.assertEqual(request.call_args_list[0].kwargs,{'tcp_only':True})
   self.assertEqual(request.call_args.args,('import',))
  self.assertEqual(H._radio_progress['stage'],'import')
  self.assertEqual(copied[0][1],'/agentpet/rss-import.json')
  self.assertEqual(copied[0][0]['items'][0]['title'],'Episode')
 def test_local_update_requires_wifi_even_with_bluetooth(self):
  with patch.object(H,'au_has_tcp',return_value=False), patch.object(H,'_ble_client',object()), patch.object(H,'au_get') as download:
   self.assertFalse(H.radio_update_local()['ok']);download.assert_not_called()
 def test_wifi_probe_failure_prevents_download(self):
  with patch.object(H,'au_has_tcp',return_value=True), patch.object(H,'radio_request',return_value={'ok':False,'why':'No response'}), patch.object(H,'au_get') as download:
   self.assertFalse(H.radio_update_local()['ok']);download.assert_not_called()
 def test_wifi_probe_uses_tcp_only(self):
  with patch.object(H,'au_has_tcp',return_value=True), patch.object(H,'push_tcp') as tcp:
   H.radio_request('status',tcp_only=True)
   tcp.assert_called_once();self.assertEqual(self.sent,[])
 def test_timeout_is_reported(self):
  self.reply.return_value=None
  self.assertFalse(H.radio_request('status')['ok'])

if __name__=='__main__':unittest.main()
