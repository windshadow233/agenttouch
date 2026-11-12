"""Mac only configures board RSS; no audio download/transcoding is started here."""
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
 def test_timeout_is_reported(self):
  self.reply.return_value=None
  self.assertFalse(H.radio_request('status')['ok'])

if __name__=='__main__':unittest.main()
