import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'runtime'))
import bootstrap
import health
import run


class RuntimeChecks(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.root=Path(self.temp.name)
        self.environ=patch.dict(os.environ,{'X4_STATE_DIR':str(self.root/'state'),
            'X4_CACHE_DIR':str(self.root/'cache'),'X4_GAME_DIR':str(self.root/'game'),
            'X4_RUNTIME_DIR':str(self.root/'runtime'),'X4_GAME_SOURCE':'mounted'},clear=True)
        self.environ.start(); self.config=bootstrap.Config()
        self.results=self.config.state/'logs'/'fixture'; self.results.mkdir(parents=True)
        (self.config.state/'current-run.json').write_text(json.dumps({'results':str(self.results)}))
    def tearDown(self): self.environ.stop(); self.temp.cleanup()

    def test_no_health_from_only_a_process_or_port(self):
        self.assertFalse(health.observe()['ready'])

    def sample(self,world):
        (self.config.profile/'host.log').write_text(f'[GPUFREE] sample game_time={world} paused=false world_ready=true moved=4\n')
        (self.results/'x4mp-perf.log').write_text('role=host fps=15 clients=0\n')

    def test_progress_and_clock_rollback_latch(self):
        self.sample(10); self.assertTrue(health.observe()['ready'])
        self.sample(11); self.assertTrue(health.observe()['ready'])
        self.sample(9); self.assertFalse(health.observe()['ready'])
        self.sample(12); self.assertFalse(health.observe()['ready'])

    def test_stale_native_update_cannot_be_ready(self):
        self.sample(10); os.utime(self.results/'x4mp-perf.log',(1,1))
        self.assertFalse(health.observe()['ready'])

    @unittest.skipIf(os.name=='nt','Linux container symlink semantics')
    def test_overlay_keeps_native_logs_persistent_and_assets_unchanged(self):
        game=self.config.game; (game/'lib').mkdir(parents=True)
        (game/'version.dat').write_text('900')
        binary=self.config.cache/'fixture'; binary.write_bytes(b'inert, never executed')
        payload=self.root/'payload'
        for name in ('gpu_free_probe','x4_host_runtime'):
            (payload/name/'ui').mkdir(parents=True)
        runtime,_,env=run.overlay(self.config,binary,self.results,payload)
        self.assertTrue((runtime/'extensions/gpu_free_probe').is_symlink())
        self.assertTrue((self.results/'extensions/gpu_free_probe/ui/gpu_free_probe_settings.lua').exists())
        self.assertFalse((game/'X4_nosteam_900').exists())
        self.assertEqual(env['X4MP_AUTO'],'')


if __name__=='__main__': unittest.main()
