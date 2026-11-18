"""No game, Steam account or external network required."""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'runtime'))
import bootstrap as boot


class BootstrapChecks(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.root=Path(self.temp.name)
        self.env={'X4_STATE_DIR':str(self.root/'state'),'X4_CACHE_DIR':str(self.root/'cache'),
                  'X4_GAME_DIR':str(self.root/'game'),'X4_GAME_SOURCE':'mounted',
                  'X4_NOSTEAM_ARCHIVE':str(self.root/'official-fixture.zip')}
        self.environment=patch.dict(os.environ,self.env,clear=True); self.environment.start()
        game=self.root/'game'; (game/'lib').mkdir(parents=True); (game/'version.dat').write_text('900')
        self.payload=b'\x7fELF'+b'inert test fixture; never executed'
        self.archive(self.payload)
        os.environ['X4_BINARY_SHA256']=hashlib.sha256(self.payload).hexdigest()

    def tearDown(self): self.environment.stop(); self.temp.cleanup()
    def archive(self,payload,name='X4'):
        with zipfile.ZipFile(self.env['X4_NOSTEAM_ARCHIVE'],'w') as z: z.writestr(name,payload)

    def test_offline_prepare_extracts_without_modifying_licensed_game(self):
        config=boot.Config(); binary=boot.prepare(config)
        self.assertEqual(binary.read_bytes(),self.payload)
        self.assertFalse((config.game/'X4_nosteam_900').exists())
        self.assertEqual(json.loads((config.state/'installation.json').read_text())['game_version'],'900')

    def test_repeat_prepare_reuses_verified_cache(self):
        config=boot.Config(); first=boot.prepare(config)
        with patch.object(boot,'fetch',side_effect=AssertionError('unexpected download')):
            self.assertEqual(boot.prepare(config),first)

    def test_bad_binary_never_promoted(self):
        self.archive(b'wrong executable')
        config=boot.Config()
        with self.assertRaises(boot.SetupError): boot.prepare(config)
        self.assertFalse((config.cache/'X4_nosteam_900').exists())

    def test_windows_archive_rejected(self):
        self.archive(b'MZ windows fixture','X4_nonsteam.exe')
        with self.assertRaises(boot.SetupError): boot.prepare(boot.Config())

    def test_unknown_game_version_cannot_launch(self):
        (self.root/'game/version.dat').write_text('1000')
        with self.assertRaises(boot.SetupError): boot.prepare(boot.Config())

    def test_secret_file_and_no_credential_in_install_manifest(self):
        password=self.root/'password'; password.write_text('sensitive-fixture\n')
        os.environ['X4_STEAM_PASSWORD_FILE']=str(password)
        config=boot.Config(); self.assertEqual(config.password,'sensitive-fixture')
        boot.prepare(config)
        self.assertNotIn('sensitive-fixture',(config.state/'installation.json').read_text())

    def test_conflicting_or_multiline_credentials_rejected(self):
        os.environ['X4_STEAM_PASSWORD']='fake\nquit'
        with self.assertRaises(boot.SetupError): boot.Config()

    def test_xml_override_preserves_unrelated_settings(self):
        config=boot.Config(); (config.profile/'config.xml').write_text('<root><custom>keep</custom></root>')
        os.environ['X4_CONFIG_JSON']='{"lodfactor":0.5,"sound":false}'
        boot.configure(config); root=ET.parse(config.profile/'config.xml').getroot()
        self.assertEqual(root.findtext('custom'),'keep'); self.assertEqual(root.findtext('lodfactor'),'0.5')
        self.assertEqual(root.findtext('sound'),'false')

    def test_native_loading_prerequisite_cannot_be_silently_broken(self):
        os.environ['X4_CONFIG_JSON']='{"uisafemode":true}'
        with self.assertRaises(boot.SetupError): boot.configure(boot.Config())

    def test_anonymous_cannot_download_licensed_game(self):
        os.environ.update(X4_GAME_SOURCE='steam',X4_STEAM_USERNAME='anonymous')
        with self.assertRaises(boot.SetupError): boot.install_game(boot.Config())

    def test_versioned_steam_branch_is_supported(self):
        os.environ['X4_STEAM_BRANCH']='9.00_legacy'
        self.assertEqual(boot.Config().branch,'9.00_legacy')

    def test_steam_script_has_no_password_in_argv_or_child_environment(self):
        os.environ.update(X4_GAME_SOURCE='steam',X4_STEAM_USERNAME='fixture',X4_STEAM_PASSWORD='fake-secret')
        config=boot.Config(); command=config.cache/'steamcmd.sh'; command.write_text('fixture')
        def run(argv,**kwargs):
            self.assertNotIn('fake-secret',' '.join(argv))
            self.assertNotIn('X4_STEAM_PASSWORD',kwargs['env'])
            script=Path(argv[-1]); self.assertIn('fake-secret',script.read_text())
            self.assertLess(script.read_text().index('force_install_dir'),script.read_text().index('login'))
            return subprocess.CompletedProcess(argv,0,b"Success! App '392160' fully installed.\n")
        with patch.object(boot,'steamcmd',return_value=command),patch.object(boot.subprocess,'run',side_effect=run):
            boot.install_game(config)
        self.assertTrue((config.cache/'game-install.json').exists())

    def test_zero_exit_without_steam_success_is_not_completed_install(self):
        os.environ.update(X4_GAME_SOURCE='steam',X4_STEAM_USERNAME='fixture')
        config=boot.Config(); command=config.cache/'steamcmd.sh'; command.write_text('fixture')
        with patch.object(boot,'steamcmd',return_value=command),patch.object(boot.subprocess,'run',return_value=subprocess.CompletedProcess([],0,b'ERROR: not logged in')):
            with self.assertRaises(boot.SetupError): boot.install_game(config)
        self.assertFalse((config.cache/'game-install.json').exists())

    def test_steam_self_update_cache_failure_has_safe_actionable_error(self):
        os.environ.update(X4_GAME_SOURCE='steam',X4_STEAM_USERNAME='fixture')
        config=boot.Config(); command=config.cache/'steamcmd.sh'; command.write_text('fixture')
        with patch.object(boot,'steamcmd',return_value=command),patch.object(boot.subprocess,'run',return_value=subprocess.CompletedProcess([],128,b'script file read error: No data available')):
            with self.assertRaisesRegex(boot.SetupError,'Linux-native'): boot.install_game(config)
        self.assertFalse((config.cache/'game-install.json').exists())

    def test_non_vendor_nosteam_download_rejected(self):
        with self.assertRaises(boot.SetupError): boot.fetch('https://untrusted.example/file.zip',self.root/'download',1024,vendor='egosoft.com')

    def test_required_dlc_checked(self):
        os.environ['X4_REQUIRED_DLC']='ego_dlc_missing'
        with self.assertRaises(boot.SetupError): boot.prepare(boot.Config())


if __name__=='__main__': unittest.main()
