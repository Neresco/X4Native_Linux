"""Build an explicit payload from public sources, without shipped binaries."""
from pathlib import Path
import shutil
import sys

source, release, output=map(Path,sys.argv[1:])
for name in ('x4native','x4mp'):
    target=output/name; target.mkdir(parents=True)
    origin=release/'x4mp_linux/extensions'/name
    for relative in ('content.xml','ui.xml','x4native.json','ui','md','t'):
        path=origin/relative
        if path.is_dir(): shutil.copytree(path,target/relative)
        elif path.is_file(): shutil.copyfile(path,target/relative)
    (target/'native').mkdir()
shutil.copyfile(source/'native/libx4native_64.so',output/'x4native/native/libx4native_64.so')
# The pinned Linux proxy expects this unprefixed filename.
shutil.copyfile(source/'native/libx4native_core.so',output/'x4native/native/x4native_core.so')
shutil.copyfile(source/'LICENSE',output/'x4native/LICENSE')
shutil.copyfile('/build/host/x4mp.so',output/'x4mp/native/x4mp.so')
shutil.copytree(source/'docker/runtime/probe',output/'gpu_free_probe')
runtime=output/'x4_host_runtime'; (runtime/'native').mkdir(parents=True); (runtime/'ui').mkdir()
(runtime/'content.xml').write_text('<content id="x4_host_runtime" name="Headless Host Runtime" version="1" author="X4MP contributors" save="0"><dependency id="x4native" optional="false"/><dependency id="gpu_free_probe" optional="false"/></content>\n')
(runtime/'x4native.json').write_text('{"library":"native/x4_host_runtime.so","priority":60,"min_api_version":1,"autoreload":false}\n')
(runtime/'ui.xml').write_text('<addon name="x4_host_runtime"><environment type="menus"><file name="ui/settings.lua"/><file name="ui/host_runtime.lua"/></environment></addon>\n')
shutil.copyfile(source/'docker/runtime/host_runtime.lua',runtime/'ui/host_runtime.lua')
shutil.copyfile('/build/x4_host_runtime.so',runtime/'native/x4_host_runtime.so')
