#!/usr/bin/env python3
"""Non-root GPU-free host supervisor. Authentication remains operator-owned."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

from bootstrap import Config, SetupError, atomic, flag, number, prepare, install_game, steamcmd


def overlay(config, binary, results, payload=Path('/opt/x4-host/extensions')):
    runtime = Path(os.environ.get('X4_RUNTIME_DIR', '/game'))
    if not runtime.is_absolute() or runtime in (config.state, config.cache, config.game):
        raise SetupError('Use a separate absolute X4_RUNTIME_DIR tmpfs.')
    runtime.mkdir(exist_ok=True)
    if any(runtime.iterdir()): raise SetupError('Runtime overlay is not empty; use a fresh private tmpfs.')
    executable = runtime / 'X4_nosteam_900'
    shutil.copy2(binary, executable)
    extensions = runtime / 'extensions'; extensions.mkdir()
    for item in config.game.iterdir():
        if item.name not in ('extensions', 'X4', 'X4_nosteam_900', 'steamapps'):
            (runtime/item.name).symlink_to(item)
    for item in (config.game/'extensions').glob('ego_dlc_*'):
        if item.is_dir(): (extensions/item.name).symlink_to(item)
    for item in payload.iterdir():
        if item.is_dir():
            # The framework writes logs and hot-load copies beside extensions.
            # Keep those with this run's evidence instead of losing them with tmpfs.
            destination=results/'extensions'/item.name
            shutil.copytree(item,destination)
            (extensions/item.name).symlink_to(destination)
    save = config.profile/'save'/(config.slot+'.xml.gz')
    settings = {'graphics':'low','upscaling':'none','start_mode':'observe' if save.exists() else 'host_existing',
                'width':config.width,'height':config.height,'frame_rate':config.fps,'gamestart':config.gamestart}
    values = ', '.join(key+' = '+json.dumps(value) for key,value in settings.items())
    (extensions/'gpu_free_probe/ui/gpu_free_probe_settings.lua').write_text('GPUFreeProbeSettings = {'+values+'}\n')
    host = extensions/'x4_host_runtime/ui/settings.lua'
    host.write_text('X4HostSettings = {slot='+json.dumps(config.slot)+', autosave='+str(config.autosave)+'}\n')
    env = os.environ.copy()
    env.update(HOME=str(config.state), XDG_CONFIG_HOME=str(config.state/'.config'),
               XDG_RUNTIME_DIR='/tmp/x4-runtime', DISPLAY=':99', SDL_AUDIODRIVER='dummy',
               X4_GAME_ROOT=str(runtime)+'/', X4_LUA_LIB=str(runtime/'lib/libluajit-5.1.so.2'),
               X4MP_AUTO='host' if save.exists() else '', X4MP_SAVE=config.slot if save.exists() else '',
               X4MP_PERF_LOG=str(results/'x4mp-perf.log'), X4_HOST_CONTROL_DIR=str(config.state/'control'),
               LP_NUM_THREADS=str(config.threads), LD_LIBRARY_PATH=str(runtime/'lib')+':'+str(runtime))
    for key in ('X4_STEAM_PASSWORD','X4_STEAM_GUARD_CODE','X4_NOSTEAM_URL'):
        env.pop(key, None)
    if config.draw:
        env.update(VK_LAYER_PATH='/opt/x4-host/draw-layer',
                   VK_INSTANCE_LAYERS='VK_LAYER_LOCAL_X4_draw_suppression', X4_DRAW_SUPPRESS='1')
    else:
        env.pop('VK_INSTANCE_LAYERS',None); env.pop('VK_LAYER_PATH',None)
    return runtime, executable, env


def display(config, env, results):
    if Path('/dev/dri').exists() or Path('/dev/dxg').exists():
        raise SetupError('GPU-free mode refuses hardware GPU device passthrough.')
    driver = next(iter(Path('/usr/share/vulkan/icd.d').glob('*lvp*.json')), None)
    if driver is None: raise SetupError('CPU Vulkan driver is missing.')
    env.update(VK_DRIVER_FILES=str(driver), VK_ICD_FILENAMES=str(driver))
    env.pop('WAYLAND_DISPLAY',None)
    Path(env['XDG_RUNTIME_DIR']).mkdir(mode=0o700, exist_ok=True)
    output = (results/'xvfb.log').open('wb')
    process = subprocess.Popen(['Xvfb',':99','-screen','0',f'{config.width}x{config.height}x24','-nolisten','tcp'],
                               env=env,stdout=output,stderr=subprocess.STDOUT)
    for _ in range(50):
        if Path('/tmp/.X11-unix/X99').exists(): break
        if process.poll() is not None: raise SetupError('Private virtual display failed to start.')
        time.sleep(.1)
    else:
        process.terminate(); process.wait(timeout=5)
        raise SetupError('Private virtual display startup timed out.')
    try:
        result = subprocess.run(['vulkaninfo','--summary'],env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=30)
        (results/'vulkan-summary.txt').write_bytes(result.stdout)
        if result.returncode or b'PHYSICAL_DEVICE_TYPE_CPU' not in result.stdout:
            raise SetupError('CPU-only Vulkan device was not confirmed.')
    except BaseException:
        process.terminate(); process.wait(timeout=5); raise
    return process


def main():
    os.umask(0o077)
    config = Config()
    mode = sys.argv[1] if len(sys.argv)>1 else 'run'
    if mode == 'auth': install_game(config, interactive=True); return 0
    if mode == 'fetch-tools':
        print('Valve SteamCMD cached at '+str(steamcmd(config))); return 0
    if mode not in ('run','prepare','diagnose'): raise SetupError('Use run, prepare, diagnose, fetch-tools or auth.')
    if mode != 'diagnose': binary = prepare(config)
    if mode != 'diagnose':
        dependencies=subprocess.run(['ldd',str(binary)],env={**os.environ,'LD_LIBRARY_PATH':str(config.game/'lib')+':'+str(config.game)},
                                    stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=20)
        atomic(config.cache/'game-dependencies.txt',dependencies.stdout)
        if dependencies.returncode or b'not found' in dependencies.stdout:
            raise SetupError('Game dynamic libraries are unavailable; inspect the private dependency report.')
    if mode == 'prepare': print('Resources, dynamic libraries and private configuration verified; X4 was not launched.'); return 0
    stamp = time.strftime('%Y%m%dT%H%M%SZ',time.gmtime())+'-'+str(os.getpid())
    results = config.state/'logs'/stamp; results.mkdir(parents=True)
    atomic(config.state/'current-run.json',json.dumps({'results':str(results),'profile':str(config.profile),'started':time.time(),'phase':mode}).encode())
    if mode == 'diagnose':
        env=os.environ.copy(); env.update(XDG_RUNTIME_DIR='/tmp/x4-runtime',DISPLAY=':99')
    else:
        runtime, executable, env = overlay(config,binary,results)
    server=display(config,env,results)
    try:
        if mode == 'diagnose':
            print('CPU Vulkan and private virtual display passed; X4 was not launched.'); return 0
        control=config.state/'control'; control.mkdir(exist_ok=True)
        for name in ('stop-requested','save-complete'):
            (control/name).unlink(missing_ok=True)
        stopped=False
        def stop(signum, frame):
            nonlocal stopped
            stopped=True
        signal.signal(signal.SIGTERM,stop); signal.signal(signal.SIGINT,stop)
        maximum=number('X4_RUN_SECONDS',1800,0,86400)
        args=json.loads(os.environ.get('X4_EXTRA_ARGS_JSON','[]'))
        if not isinstance(args,list) or any(not isinstance(arg,str) for arg in args):
            raise SetupError('X4_EXTRA_ARGS_JSON must be an array of strings.')
        game_args=[str(executable),'-nologo','-nosound','-windowed','-nocputhrottle','-skipintro','-debug','scripts','-logfile','host.log',*args]
        started=time.monotonic()
        with (results/'game-stdout.log').open('wb') as output:
            game=subprocess.Popen(['nice','-n','10',*game_args],cwd=runtime,env=env,stdout=output,stderr=subprocess.STDOUT,start_new_session=True)
            while game.poll() is None and not stopped and (not maximum or time.monotonic()-started<maximum):
                time.sleep(.5)
            saved=False
            if game.poll() is None:
                (control/'stop-requested').touch()
                deadline=time.monotonic()+config.stop_wait
                while game.poll() is None and time.monotonic()<deadline:
                    if (control/'save-complete').exists(): saved=True; break
                    time.sleep(.5)
                game.terminate()
                try: game.wait(timeout=15)
                except subprocess.TimeoutExpired: game.kill(); game.wait(timeout=5)
            atomic(results/'exit.json',json.dumps({'exit_code':game.returncode,'save_acknowledged':saved,'bounded':bool(maximum)}).encode())
            print('Host exited; save acknowledgement='+str(saved).lower())
            return game.returncode
    finally:
        server.terminate()
        try: server.wait(timeout=5)
        except subprocess.TimeoutExpired: server.kill(); server.wait(timeout=5)


if __name__ == '__main__':
    try: raise SystemExit(main())
    except (SetupError,OSError,ValueError,subprocess.SubprocessError) as error:
        # Unexpected OS/parser failures may include signed URLs/secret paths;
        # only our explicit, non-secret setup messages enter container output.
        print('X4 host setup failed: '+(str(error) if isinstance(error,SetupError) else 'resource/configuration operation failed.'),file=sys.stderr)
        raise SystemExit(1)
