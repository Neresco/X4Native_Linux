"""Acquire licensed resources at runtime; never put game/auth data in an image."""
import hashlib
import http.cookiejar
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

STEAMCMD_URL = 'https://steamcdn-a.akamaihd.net/client/installer/steamcmd_linux.tar.gz'
OFFICIAL_NOSTEAM = 'https://www.egosoft.com/download/x4/bonus_en.php'
VALIDATED_BINARY = '29c0d9ba16685ab1bf813fbabc2a4806e73acd60b610e5b8887f13ab7f4fcbf1'


class SetupError(Exception):
    pass


def flag(name, default=False):
    value = os.environ.get(name, '1' if default else '0').lower()
    if value not in ('1', '0', 'true', 'false'):
        raise SetupError(name + ' must be 0/1 or true/false.')
    return value in ('1', 'true')


def number(name, default, minimum, maximum):
    value = os.environ.get(name, str(default))
    if not value.isdecimal() or not minimum <= int(value) <= maximum:
        raise SetupError(name + f' must be {minimum}..{maximum}.')
    return int(value)


def secret(name):
    value, file = os.environ.get(name), os.environ.get(name + '_FILE')
    if value is not None and file:
        raise SetupError('Use only one of ' + name + ' and ' + name + '_FILE.')
    if file:
        try: value = Path(file).read_text(encoding='utf-8').rstrip('\r\n')
        except OSError: raise SetupError(name + '_FILE could not be read; check the private secret mount.') from None
    if value is not None and ('\n' in value or '\r' in value or '\0' in value or len(value) > 8192):
        raise SetupError(name + ' contains unsupported control characters or is too long.')
    return value or ''


def atomic(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    pending = path.with_name(path.name + '.next')
    pending.write_bytes(content)
    pending.chmod(0o600)
    pending.replace(path)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


class Config:
    def __init__(self):
        self.state = Path(os.environ.get('X4_STATE_DIR', '/state'))
        self.cache = Path(os.environ.get('X4_CACHE_DIR', '/cache'))
        self.game = Path(os.environ.get('X4_GAME_DIR', '/data/game'))
        if not all(p.is_absolute() for p in (self.state, self.cache, self.game)):
            raise SetupError('State, cache and game directories must be absolute.')
        if len({p.resolve() for p in (self.state, self.cache, self.game)}) != 3:
            raise SetupError('State, cache and game directories must be separate.')
        self.profile = self.state / '.config/EgoSoft/X4'
        self.source = os.environ.get('X4_GAME_SOURCE', 'steam')
        if self.source not in ('steam', 'mounted'):
            raise SetupError('X4_GAME_SOURCE must be steam or mounted.')
        self.version = os.environ.get('X4_GAME_VERSION', '900')
        if self.version != '900':
            raise SetupError('This native SDK/package currently validates game version 900 only.')
        self.update = os.environ.get('X4_STEAM_UPDATE', 'missing')
        if self.update not in ('missing', 'always', 'never'):
            raise SetupError('X4_STEAM_UPDATE must be missing, always or never.')
        self.branch = os.environ.get('X4_STEAM_BRANCH', 'public')
        if not re.fullmatch(r'[A-Za-z0-9_.-]{1,100}', self.branch):
            raise SetupError('Invalid Steam branch name.')
        self.username = secret('X4_STEAM_USERNAME')
        self.password = secret('X4_STEAM_PASSWORD')
        self.guard = secret('X4_STEAM_GUARD_CODE')
        self.nosteam_url = secret('X4_NOSTEAM_URL')
        self.binary_hash = os.environ.get('X4_BINARY_SHA256', VALIDATED_BINARY)
        if not re.fullmatch('[0-9a-f]{64}', self.binary_hash):
            raise SetupError('X4_BINARY_SHA256 must be a SHA-256 digest.')
        self.slot = os.environ.get('X4_SAVE', 'save_001')
        if not re.fullmatch(r'save_[0-9]{3}', self.slot):
            raise SetupError('X4_SAVE must use an X4 slot such as save_001.')
        self.gamestart = os.environ.get('X4_NEW_GAME', 'x4ep1_gamestart_trade')
        if not re.fullmatch('[A-Za-z0-9_]{1,100}', self.gamestart):
            raise SetupError('Invalid X4_NEW_GAME identifier.')
        self.width = number('X4_WIDTH', 1280, 640, 1920)
        self.height = number('X4_HEIGHT', 720, 360, 1080)
        self.fps = number('X4_FRAME_RATE', 15, 1, 60)
        self.threads = number('X4_RENDER_THREADS', 2, 1, 32)
        self.autosave = number('X4_AUTOSAVE_SECONDS', 300, 30, 86400)
        self.stop_wait = number('X4_SHUTDOWN_SAVE_TIMEOUT', 60, 0, 300)
        self.draw = flag('X4_DRAW_SUPPRESS', True)
        self.profile.mkdir(parents=True, exist_ok=True)
        self.cache.mkdir(parents=True, exist_ok=True)


def fetch(url, target, limit, cookie_file='', vendor=None):
    def allowed(address):
        parsed = urllib.parse.urlsplit(address)
        host = (parsed.hostname or '').lower()
        return parsed.scheme == 'https' and not parsed.username and (
            not vendor or host == vendor or host.endswith('.' + vendor))
    if not allowed(url):
        raise SetupError('Download URL must use HTTPS and the expected vendor host.')
    class Redirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, request, response, code, message, headers, newurl):
            if not allowed(newurl): raise SetupError('Download redirected outside the approved vendor.')
            return super().redirect_request(request, response, code, message, headers, newurl)
    handlers = [Redirect()]
    if cookie_file:
        jar = http.cookiejar.MozillaCookieJar(cookie_file)
        jar.load(ignore_discard=True, ignore_expires=False)
        handlers.append(urllib.request.HTTPCookieProcessor(jar))
    opener = urllib.request.build_opener(*handlers)
    pending = target.with_name(target.name + '.incoming')
    count = 0
    try:
        with opener.open(urllib.request.Request(url, headers={'User-Agent': 'X4MP-Host/0.2'}), timeout=30) as response, pending.open('wb') as output:
            while data := response.read(1024 * 1024):
                count += len(data)
                if count > limit: raise SetupError('Download exceeds its configured resource limit.')
                output.write(data)
        pending.replace(target)
    except SetupError:
        raise
    except Exception:
        # Never echo a signed URL, cookie, credential or downloader exception.
        raise SetupError('Vendor download failed. Check authorized access and network availability.') from None


def steamcmd(config):
    directory = config.cache / 'steamcmd'
    directory.mkdir(exist_ok=True)
    command = directory / 'steamcmd.sh'
    if command.exists(): return command
    archive = config.cache / 'steamcmd_linux.tar.gz'
    fetch(STEAMCMD_URL, archive, 64 * 1024**2)
    expected = os.environ.get('X4_STEAMCMD_SHA256')
    if expected and digest(archive) != expected:
        raise SetupError('SteamCMD archive checksum mismatch.')
    with tarfile.open(archive) as bundle:
        members = bundle.getmembers()
        if sum(m.size for m in members) > 256 * 1024**2 or any(
                m.issym() or m.islnk() or not (m.isfile() or m.isdir()) or
                PurePosixPath(m.name).is_absolute() or '..' in PurePosixPath(m.name).parts for m in members):
            raise SetupError('Unsafe SteamCMD archive.')
        bundle.extractall(directory, filter='data')
    if not command.exists(): raise SetupError('SteamCMD archive is missing its launcher.')
    command.chmod(0o700)
    return command


def install_game(config, interactive=False):
    present = (config.game / 'version.dat').exists()
    completed = config.cache / 'game-install.json'
    if config.source == 'mounted':
        if not present: raise SetupError('Mounted licensed Linux game data is missing.')
        return
    if present and completed.exists() and config.update != 'always' and not interactive: return
    if config.update == 'never' and not interactive:
        raise SetupError('Game data is missing and Steam downloading is disabled.')
    if not config.username or config.username.lower() == 'anonymous':
        raise SetupError('Set X4_STEAM_USERNAME for an account owning X4; anonymous installation is unavailable.')
    command = steamcmd(config)
    config.game.mkdir(parents=True, exist_ok=True)
    home = config.cache / 'steam-home'; home.mkdir(exist_ok=True)
    env = os.environ.copy(); env['HOME'] = str(home)
    for key in ('X4_STEAM_PASSWORD', 'X4_STEAM_GUARD_CODE', 'X4_NOSTEAM_URL'):
        env.pop(key, None)
    if interactive:
        # Authentication is deployment setup in the owner's terminal, not a
        # player-facing multiplayer workflow. No desktop automation is used.
        subprocess.run([str(command), '+login', config.username, '+quit'], env=env, cwd=command.parent, check=True)
        return
    def quote(value):
        return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'
    credentials = [config.username]
    if config.password or config.guard: credentials.append(config.password)
    if config.guard: credentials.append(config.guard)
    login = 'login ' + ' '.join(quote(v) for v in credentials)
    update = 'app_update 392160'
    if config.branch != 'public': update += ' -beta ' + config.branch
    if flag('X4_STEAM_VALIDATE', True): update += ' validate'
    lines = ['@ShutdownOnFailedCommand 1', '@NoPromptForPassword 1',
             '@sSteamCmdForcePlatformType linux', 'force_install_dir ' + quote(str(config.game)), login, update, 'quit']
    # Credentials live only in a mode-0600 tmpfs script, never argv/image/logs.
    with tempfile.TemporaryDirectory(prefix='x4-steam-') as temporary:
        script = Path(temporary) / 'install.txt'; script.write_text('\n'.join(lines) + '\n'); script.chmod(0o600)
        result = subprocess.run([str(command), '+runscript', str(script)], env=env, cwd=command.parent,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=number('X4_DOWNLOAD_TIMEOUT', 7200, 60, 86400))
    if b'script file read error' in result.stdout:
        raise SetupError('SteamCMD self-update could not read its cache files. Use Linux-native cache storage and retry prepare.')
    if result.returncode or not re.search(rb"Success!\s+App\s+'392160'\s+fully installed", result.stdout) or not (config.game / 'version.dat').exists():
        raise SetupError('Steam acquisition failed. Complete Steam Guard using auth mode, then retry.')
    atomic(completed, json.dumps({'app':392160,'branch':config.branch,'platform':'linux'}).encode())


def executable(config):
    target = config.cache / ('X4_nosteam_' + config.version)
    if target.exists() and digest(target) == config.binary_hash: return target
    archive = Path(os.environ.get('X4_NOSTEAM_ARCHIVE', '/downloads/X4_9.00_nonsteam_Linux.zip'))
    if not archive.exists():
        if not config.nosteam_url:
            raise SetupError('Official NoSteam access is required: set X4_NOSTEAM_URL and optional X4_EGOSOFT_COOKIE_FILE, or mount X4_NOSTEAM_ARCHIVE. Registered ownership is required by Egosoft.')
        archive = config.cache / 'nosteam-linux.zip'
        fetch(config.nosteam_url, archive, 64 * 1024**2, os.environ.get('X4_EGOSOFT_COOKIE_FILE', ''), 'egosoft.com')
    try:
        with zipfile.ZipFile(archive) as bundle:
            entries = [m for m in bundle.infolist() if PurePosixPath(m.filename).name == 'X4' and not m.is_dir()]
            if len(entries) != 1 or entries[0].file_size > 256 * 1024**2 or '..' in PurePosixPath(entries[0].filename).parts:
                raise SetupError('Unexpected official NoSteam Linux archive layout.')
            pending = target.with_name(target.name + '.incoming')
            with bundle.open(entries[0]) as source, pending.open('wb') as output:
                shutil.copyfileobj(source, output, 1024 * 1024)
        with pending.open('rb') as stream: magic = stream.read(4)
        if magic != b'\x7fELF' or digest(pending) != config.binary_hash:
            raise SetupError('Official NoSteam Linux executable checksum/format mismatch.')
        pending.chmod(0o700); pending.replace(target)
    except (zipfile.BadZipFile, KeyError):
        raise SetupError('NoSteam download was not a valid Linux ZIP; authenticated vendor access may be missing.') from None
    return target


def configure(config):
    path = config.profile / 'config.xml'
    tree = ET.parse(path) if path.exists() else ET.ElementTree(ET.Element('root'))
    values = dict(fullscreen=False, res_width=config.width, res_height=config.height,
                  antialiasing='none', gfxquality=1, shadows=0, softshadows=False,
                  ssao=0, volumetric=0, uisafemode=False, frameratelimit=True,
                  frameratetarget=config.fps, skipintro=True)
    extra = json.loads(os.environ.get('X4_CONFIG_JSON', '{}'))
    if not isinstance(extra, dict) or any(not re.fullmatch('[A-Za-z_][A-Za-z0-9_]*', key) or
            not isinstance(value, (str, int, float, bool)) for key, value in extra.items()):
        raise SetupError('X4_CONFIG_JSON must map simple XML tag names to scalar values.')
    values.update(extra)
    if str(values['uisafemode']).lower() != 'false':
        raise SetupError('Native host loading requires Protected UI Mode disabled in its private profile.')
    for key, value in values.items():
        element = tree.getroot().find(key)
        if element is None: element = ET.SubElement(tree.getroot(), key)
        element.text = str(value).lower() if isinstance(value, bool) else str(value)
    atomic(path, ET.tostring(tree.getroot(), encoding='utf-8', xml_declaration=True))


def prepare(config):
    install_game(config)
    version = (config.game / 'version.dat').read_text().strip()
    if version != config.version: raise SetupError('Downloaded game version differs from the pinned native/NoSteam build. Select a matching Steam branch; no game was launched.')
    build = os.environ.get('X4_STEAM_BUILD_ID')
    if build:
        manifest = (config.game / 'steamapps/appmanifest_392160.acf').read_text()
        if not re.search(r'"buildid"\s+"' + re.escape(build) + '"', manifest):
            raise SetupError('Steam build ID differs from X4_STEAM_BUILD_ID.')
    if not (config.game / 'lib').is_dir(): raise SetupError('Linux game libraries are missing.')
    required = [v for v in os.environ.get('X4_REQUIRED_DLC', '').split(',') if v]
    if any(not re.fullmatch('ego_dlc_[A-Za-z0-9_]+', name) or not (config.game/'extensions'/name).is_dir() for name in required):
        raise SetupError('Required owned DLC is missing from this installation.')
    binary = executable(config)
    configure(config)
    atomic(config.state/'installation.json', json.dumps({'game_version': version, 'binary_sha256': digest(binary),
           'source': config.source, 'dlc': sorted(p.name for p in (config.game/'extensions').glob('ego_dlc_*') if p.is_dir())}).encode())
    return binary
