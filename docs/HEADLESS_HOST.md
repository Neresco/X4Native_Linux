# GPU-free Docker host (experimental)

This package runs the existing **Linux X4 9.00 engine** as a multiplayer host.
It is not a standalone simulation server or a complete renderer bypass. Mesa
CPU Vulkan and a private virtual display remain necessary. Native host/client
work is under active development; see [validation](VALIDATION.md) and
[design decisions](MULTIPLAYER_DESIGN.md) before treating it as a deployment.

## Resource ownership

The image builds the native framework/host/control plugin and experimental draw
layer from source. It downloads the pinned public Linux extension payload during
the build. OS packages and SteamCMD dependencies are installed by the image.
Licensed game/DLC data, Steam state, NoSteam files, cookies, saves and credentials
are acquired or supplied **at container runtime in private mounts**, not images.

SteamCMD can download owned Linux content using app 392160. A valid owning Steam
account is required; this is not an anonymously downloadable dedicated server.
Steam Guard remains an owner-operated authentication step. Egosoft also restricts
the official NoSteam executable to registered owners/linked Steam accounts.
Provide an authorized vendor download URL/cookie file or a legitimately obtained
Linux archive. The package does not bypass those gates or accept an unofficial
game executable. Unknown game versions fail before launch.

## Build and configure

From the native repository root:

```sh
docker build -f docker/Dockerfile -t x4-headless-host:development .
cp docker/.env.example docker/.env
```

Edit `docker/.env` for your Steam account and private storage/download/secrets
paths. Create bind directories with permissions for the configured numeric UID/GID.
Use Linux-native storage for the Steam cache. A Windows-backed cache hit a
self-updating launcher read error during a development check; the updated tool
started successfully on Linux-native temporary storage. Failed/self-update-only
Steam runs never count as a completed game installation.
Keep `.env`, the Steam cache and secret files private. Prefer `_FILE` credentials
over plaintext environment values, which container inspection can expose.

Authentication and preparation are operator setup, not external player controls:

```sh
docker compose --env-file docker/.env -f docker/compose.yaml run --rm host auth
docker compose --env-file docker/.env -f docker/compose.yaml run --rm host prepare
docker compose --env-file docker/.env -f docker/compose.yaml run --rm host diagnose
```

`auth` runs SteamCMD interactively in the owner's terminal; never paste credentials
into chat or issue/PR logs. `prepare` downloads/verifies the licensed resources and
generates the private game config without starting X4. `diagnose` checks software
Vulkan and the private display without starting X4. `fetch-tools` downloads only
public SteamCMD tooling. A full licensed Steam install still needs account access.

When the deployment/network policy has been reviewed, the normal command is:

```sh
docker compose --env-file docker/.env -f docker/compose.yaml up host
```

The example publishes no ports, mounts no GPU/display/Docker socket and creates
no automatic restart policy. It defaults to two CPU units/12 GiB/no swap and a
30-minute engine run limit, plus bounded save/stop grace. Resource acquisition
has its own download timeout. Do not assume a runtime cap also bounds image
build/import or initial game-transfer impact on a shared machine.

## Configuration

All settings below are environment variables; native `X4MP_*` knobs also pass
through, except role/save selection and the per-run performance log, which the
entrypoint derives from the selected host save/run. Docker bind paths and resource limits are
Compose substitutions. No game-client files need manual editing for these options.

| Variable | Default / meaning |
| --- | --- |
| `X4_GAME_SOURCE` | `steam`; alternatively `mounted` for existing licensed Linux data |
| `X4_GAME_DIR`, `X4_STATE_DIR`, `X4_CACHE_DIR` | `/data/game`, `/state`, `/cache`; separate absolute private locations |
| `X4_STEAM_USERNAME`, `X4_STEAM_PASSWORD`, `X4_STEAM_GUARD_CODE` | Steam account inputs; each supports `_FILE`; password/code never enter argv or image layers |
| `X4_STEAM_UPDATE` | `missing`, `always` or `never`; successful installs are cached; interrupted/failed installs are not complete |
| `X4_STEAM_BRANCH`, `X4_STEAM_VALIDATE` | `public`, `1`; select a matching owned branch and validate acquired files |
| `X4_STEAM_BUILD_ID` | Optional exact Steam manifest build pin |
| `X4_GAME_VERSION` | `900`; other ABI/game versions currently rejected |
| `X4_DOWNLOAD_TIMEOUT` | Steam acquisition deadline, default 7200 seconds |
| `X4_STEAMCMD_SHA256` | Optional checksum pin for the official SteamCMD bootstrap archive |
| `X4_NOSTEAM_URL` / `_FILE` | Owner-authorized HTTPS download on egosoft.com; no URL/token is logged |
| `X4_EGOSOFT_COOKIE_FILE` | Optional owner-supplied Netscape-format vendor cookie jar |
| `X4_NOSTEAM_ARCHIVE` | `/downloads/X4_9.00_nonsteam_Linux.zip`; runtime archive fallback |
| `X4_BINARY_SHA256` | Default validated official 9.00 Linux executable digest; override only after independent matching-build validation |
| `X4_REQUIRED_DLC` | Optional comma-separated owned `ego_dlc_*` directories required before launch |
| `X4_SAVE`, `X4_NEW_GAME` | `save_001`, `x4ep1_gamestart_trade`; reload existing canonical slot, otherwise bootstrap a new world |
| `X4_AUTOSAVE_SECONDS` | 300; first automatic save is requested 30 seconds after definitive readiness |
| `X4_SHUTDOWN_SAVE_TIMEOUT` | 60; wait for real game-saved acknowledgement before terminating; zero skips waiting |
| `X4_RUN_SECONDS` | 1800; engine time cap, zero explicitly requests no cap |
| `X4_WIDTH`, `X4_HEIGHT`, `X4_FRAME_RATE` | 1280, 720, 15; validated private graphics configuration |
| `X4_RENDER_THREADS` | 2; software-rendering worker count, not a total CPU quota |
| `X4_DRAW_SUPPRESS` | 1 in the example; experimental Vulkan draw suppression still initializes the renderer |
| `X4_CONFIG_JSON` | Additional scalar X4 XML settings; existing unrelated values preserved; Protected UI Mode must remain disabled |
| `X4_EXTRA_ARGS_JSON` | Optional JSON string array of game CLI arguments; no shell evaluation |
| `X4MP_TRANSPORT`, `X4MP_LEGACY_NET` | `tcp`, `0`; consolidated native protocol |
| `X4MP_PORT`, `X4MP_STREAM_PORT` | 7777, 7778; these are not automatically published |
| `X4MP_STREAMS`, `X4MP_STREAMSHIPS`, `X4MP_FULLSIM` | 0; preserve normal attention/detail behavior |
| `X4_STORAGE_ROOT`, `X4_DOWNLOADS_ROOT`, `X4_SECRET_ROOT` | Compose host-side bind roots; use a private fast-storage directory |
| `X4_CPU_LIMIT`, `X4_MEMORY_LIMIT` | Compose defaults 2 and 12g; memory-swap equals memory |
| `X4_UID`, `X4_GID`, `X4_STOP_GRACE`, `X4_IMAGE` | Compose operator settings; default 1000/100/90s/development image |

## Persistence, health and stop

The original Steam executable is preserved. A verified official NoSteam binary
is cached separately and copied into an ephemeral overlay, with symlinks to
licensed assets and fresh writable native-extension copies. A read-only mounted
game tree is supported. Saves/configs remain in the state mount across containers.
Runtime logs/health are stored per run; a failed run does not overwrite its predecessor.

Health requires unpaused advancing game time, sampled NPC movement, a real world
ready signal and fresh native host updates. A clock rollback latches unhealthy
for that run. A listening port, running PID or player ID is insufficient.

SIGTERM requests a save through a native UI-frame callback and Lua's delayed
game-thread lifecycle. A completed game-save event acknowledges it. The supervisor
waits only the configured grace, then terminates its own game/display processes;
it never claims an unacknowledged save succeeded. The new managed-image save/stop
and restart paths still need real-engine acceptance; mocked installer/unit checks
are not proof of persistent universe correctness.

## Known gaps

The new managed package is a draft. Earlier bounded software-rendered host runs
established feasibility, but connected gameplay, occupied-sector capacity, economy
transactions, robust save/recovery and production operation are unqualified.
Admission/admin roles and public authentication are not implemented by this image.
The server owns the universe; player and admin controls belong inside X4 clients.
Do not expose an unauthenticated native listener as a production Internet service.

Primary references: [Valve SteamCMD](https://developer.valvesoftware.com/wiki/SteamCMD),
[Valve install-directory ordering](https://github.com/ValveSoftware/steam-for-linux/issues/8298),
[Egosoft NoSteam access](https://www.egosoft.com/download/x4/bonus_en.php),
[Mesa software Vulkan](https://docs.mesa3d.org/drivers/lavapipe.html).
