"""Release identity and structural verifier gates, independent of an ESP build."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]

def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

version = load('git_branch')
security = load('check_ota_security')

class Env(dict):
    def Append(self, **values):
        self.update(values)

class ReleaseMetadataTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / 'platformio.ini').write_text('[inkademic]\nversion = 1.8.0-RC\n')

    def emitted(self, target):
        env = Env(PROJECT_DIR=str(self.root), PIOENV=target)
        version.inject_version(env)
        return env['CPPDEFINES'][0][1].replace('\\"', '')

    def test_rc_metadata_consistent_on_all_hardware(self):
        with patch.dict(os.environ, {'INKADEMIC_RC_HASH':'abc1234', 'INKADEMIC_RC_ARTIFACTS':'1'}, clear=True):
            for target in ('default','sticky','x4-pro','gh_release_rc'):
                self.assertEqual(self.emitted(target), '1.8.0-rc+abc1234')

    def test_rc_takes_priority_over_legacy_boolean_release_flag(self):
        with patch.dict(os.environ, {'INKADEMIC_RC_HASH':'abc1234','INKADEMIC_RELEASE_VERSION':'1'}, clear=True):
            for target in ('default','sticky','x4-pro'):
                self.assertEqual(self.emitted(target),'1.8.0-rc+abc1234')

    def test_production_metadata_exact_on_all_hardware(self):
        with patch.dict(os.environ, {'INKADEMIC_RELEASE_VERSION':'v1.8.1'}, clear=True):
            for target in ('default','sticky','x4-pro'):
                self.assertEqual(self.emitted(target),'1.8.1')

    def test_local_device_builds_identify_development(self):
        (self.root/'platformio.ini').write_text('[inkademic]\nversion = 1.8.0\n')
        with patch.dict(os.environ, {}, clear=True):
            for target in ('sticky','x4-pro'):
                self.assertEqual(self.emitted(target), '1.8.0-dev+'+target)

    def test_security_gate_requires_both_key_and_real_symbols(self):
        import re
        header = (ROOT/'include/OtaUpdatePublicKey.h').read_text()
        key = bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',header))
        symbols = '4001 T wc_ed25519_verify_msg\n4002 T wc_ed25519_import_public\n'
        security.check_image(b'header'+key+b'tail',header,symbols)
        for image,symbols_bad in [(b'unsigned',symbols),(key,''),(key,'4001 U wc_ed25519_verify_msg\n4002 T wc_ed25519_import_public\n')]:
            with self.assertRaises(RuntimeError): security.check_image(image,header,symbols_bad)

if __name__=='__main__': unittest.main()
