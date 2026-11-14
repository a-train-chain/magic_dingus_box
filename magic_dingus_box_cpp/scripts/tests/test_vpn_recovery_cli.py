import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from vpn_recovery.common import Paths,atomic_json
from vpn_recovery.intent import read_intent,set_intent
try:
    from vpn_recovery.cli import main
except ImportError: main=None

class CLITests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(main,'recovery CLI not implemented')
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        root=Path(self.tmp.name);self.paths=Paths(base=root,state=root/'state',runtime=root/'run',compose_lock=root/'lock')
        self.paths.services.mkdir();(self.paths.services/'.env').write_text('VPN_PORT_FORWARDING=on\n')
        self.args=['--base',str(root),'--state',str(root/'state'),'--runtime',str(root/'run'),'--compose-lock',str(root/'lock')]
    def call(self,*args):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            code=main(self.args+list(args))
        return code,output.getvalue()
    def test_intent_and_inhibit_commands(self):
        self.assertEqual(self.call('intent','enable','--reason','setup')[0],0)
        self.assertEqual(self.call('inhibit','begin','test','--owner-pid',str(os.getpid()),'--reason','maintenance')[0],0)
        self.assertIn('test',read_intent(self.paths)['inhibits'])
        self.call('intent','stop','--reason','reset')
        self.call('inhibit','end','test')
        self.assertFalse(read_intent(self.paths)['enabled'])
    def test_run_inhibited_releases_on_child_failure_and_preserves_stop(self):
        set_intent(self.paths,False,'stop')
        with patch('vpn_recovery.cli.SystemAdapters') as adapters:
            adapters.return_value.current.return_value={}
            self.assertEqual(self.call('run-inhibited','--reason','test','--',sys.executable,'-c','raise SystemExit(7)')[0],7)
        self.assertFalse(read_intent(self.paths)['enabled']);self.assertFalse(read_intent(self.paths)['inhibits'])
    def test_stale_status_never_ready(self):
        atomic_json(self.paths.state/'status.json',{'ready':True,'checked_at':'2000-01-01T00:00:00+00:00','traffic_verified':True})
        code,out=self.call('status');self.assertEqual(code,0)
        self.assertFalse(json.loads(out)['ready'])
    def test_supported_policy_and_invalid_fails_distinctly(self):
        self.assertEqual(self.call('supported')[0],0)
        (self.paths.services/'.env').write_text('VPN_SERVICE_PROVIDER=custom\n')
        self.assertEqual(self.call('supported')[0],1)
        (self.paths.services/'.env').write_text('VPN_TYPE=invalid\n')
        self.assertEqual(self.call('supported')[0],2)

if __name__=='__main__':unittest.main()
