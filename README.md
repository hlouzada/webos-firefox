# Firefox for webOS TV

Native browser for the OLED55C56LB (webOSTV 25, platform 10.3.1, firmware 33.31.68.01). Userspace is 32-bit ARM, softfp, glibc 2.35. The on-TV name is Firefox. The app id stays `com.github.gprot42.geckotv`, so an existing install upgrades in place. To put it on a TV, see [INSTALL-WEBOS.md](INSTALL-WEBOS.md).

Firefox is a trademark of the Mozilla Foundation. This packages a Firefox ESR build for personal use on one TV and ships its own icon artwork, not Mozilla's. The build carries three small build fixes in `build/patches/`.

`app/geckotv` runs `app/firefox-runtime/firefox` when that file is present. That runtime is Firefox ESR 153.3 built as a native 32-bit program (ARMv7, NEON, softfp), the same ABI as the TV's userspace. It runs on the TV's own glibc (2.30 or newer) and needs nothing else installed. libstdc++ is linked statically with its symbols hidden (`build/mozconfig-arm32`, `build/stdcxx-static/`), so Firefox does not depend on the TV's C++ library version; the Mali driver keeps using the TV's copy. Rust is built for the stock `armv7-unknown-linux-gnueabi` target with the FPU features switched on, so it uses the hardware FPU; `findings.md` section 12 records how that was proven on the TV. Firefox renders on the Mali-G52 through GPU WebRender. On webOS 5 and 6 (flat mode, `findings.md` section 19) the adapter runs in GL window mode. Firefox's GL content surface is the window the compositor shows. Menus and drop-downs are copied into a transparent OVERLAY window of the app above it, because the compositor never draws surfaces imported through `wl_webos_foreign`. A pop-up that Firefox draws with the GPU gets a FLOATING window of its own with its GL buffers passed through. The add-on install prompt and the other popup notifications are such pop-ups, since their panel holds remote content. An OVERLAY window mapped after launch would take the focus and get the app closed. The FLOATING window is kept hidden behind a transparent buffer rather than destroyed, because destroying a window of the app minimizes the card. The launcher links `libwayland-egl.so.1` to the TV's own `libwayland-egl.so.0`, the library the Mali driver was built with. `GECKOTV_SOFTWARE=1` in the `env` file switches back to software rendering with the flat window that copies frames on the CPU. Scrolling a long page at 720p, it delivers 125 frames a second on about 105% of one core, where software rendering gives 88 frames on 125%. `findings.md` section 13 has the details and the four fixes GPU rendering needed.

An earlier 64-bit build ran through the `org.webosbrew.bridge-64to32` loader with software rendering only. It has been removed; `findings.md` keeps its measurements.

Until the runtime exists, the same launcher runs `app/smoke`, a Wayland client that paints a fullscreen GLES frame.

`findings.md` records what was measured on the device: the compositor behaviour behind the window bugs, why each fix is shaped the way it is, the video and memory numbers, and the diagnostic traps worth knowing before debugging this thing.

## Build the smoke IPK

The compiler is the webOS SDK at `/Users/aicoder/toolchains/arm-webos-linux-gnueabi_sdk-buildroot` (GCC 12.2). Its sysroot glibc is 2.12.2, which is old enough for this small client and the wrong sysroot for Firefox.

```sh
make
python3 scripts/pack-ipk.py
```

`scripts/pack-ipk.py` raises the last number of the version in `app/appinfo.json` on every run (0.1.4 becomes 0.1.5), because webOS may skip installing a package whose version is already installed; `--no-bump` repacks at the current version.

`scripts/install2tvfrommacos.sh` builds, packages, and installs to `root@192.168.0.79` with `~/.ssh/webos_deploy` when the TV answers. Ares device name `webos`.

To open a page at launch, pass it as the webOS `target` parameter (http and https only):

```sh
ares-launch -d webos com.github.gprot42.geckotv -p '{"target":"https://www.youtube.com/"}'
```

After launch, the log is on the TV at:

```
/media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv/geckotv.log
```

## Window size

With GPU rendering the window is 1920x1080. With software rendering (`GECKOTV_SOFTWARE=1`, or a TV without `libwayland-egl`) the adapter tells Firefox its window is 1280x720 and the compositor scales it to the panel, because at 1080p the software path cost the parent process ~170% CPU and dropped a quarter of video frames. `WEBOS_XDG_SIZE=WxH` in the `env` file overrides either.

## Codecs

Firefox loads the TV's own FFmpeg 5.0 (`libavcodec.so.59`), so `about:support` lists H.264, HEVC, VP8, VP9, AV1, AAC, MP3, Opus, Vorbis, FLAC and Wave as software decoders (SWDEC). No FFmpeg is bundled. On webOS 25 every frame is decoded on the CPU, because the TV has no VA-API driver. On webOS 5 and 6 in GL window mode, H.264 and VP9 video with AAC or no audio plays through LG's media pipeline (Starfish, `libplayerAPIs.so.1`). Firefox's webOS media engine in `build/patches/webos-media-engine.patch` is an `ExternalPlaybackEngine` for Firefox's `ExternalEngineStateMachine`. It hands the compressed samples to a helper process (`src/media-helper.cc`, a mode of the launcher). The pipeline decodes, paces and plays the audio on the hardware video plane under the window, and Firefox clears the window to alpha 0 where the video is (a WebRender cut-out rectangle) so the plane shows through with the page's controls over it. Anything the pipeline cannot take plays in software as before. `MediaSource.isTypeSupported` refuses other codecs while the engine is available, so a site such as YouTube negotiates streams the pipeline plays. `findings.md` section 21 explains the design, the facts it rests on and what to check on a TV. `GECKOTV_HWVIDEO=0` in the `env` file turns it off. The host tests are `tests/media-helper/run.sh` and `tests/adapter/run.sh`.

## Remote control for debugging

Firefox starts without Marionette. To drive it from the Mac, create the marker file and relaunch:

```
touch /media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv/marionette
```

Delete it afterwards. With Marionette on, `navigator.webdriver` is true on every page, and YouTube then stops sending video after about a minute and shows "Something went wrong"; see `findings.md` section 8.

## Reading a crash

`webos-xdg: signal 11 addr=(nil)` with `g_log_structured_standard` and `gdk_event_source_check` in the backtrace is not a Firefox bug. GDK saw the Wayland socket die and Firefox's GLib log writer turned that into a `MOZ_CRASH`, which prints nothing in a release build. It means surface-manager restarted. Check `/var/log/reports/librdx` for the compositor's own report and the `NL_VSC` lines in `/var/log/messages*.gz` for which card LSM showed.

To capture the wire protocol for one run, create the marker file, launch, then delete it:

```
touch /media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv/wayland-debug
```

The launcher then sets `WAYLAND_DEBUG=1`, and libwayland logs every request and event into `geckotv.log`.

How much the adapter and Firefox write to `geckotv.log` is fixed at build time. `make LOGLEVEL=1 app/geckotv` and `LOGLEVEL=1 build/build-adapter.sh` build a debugging pair. The adapter then logs every listener, key, pointer and focus event (several hundred lines per start) and the launcher writes `user.js` prefs that let Firefox dump chrome console messages and CSS errors to the log. The default `LOGLEVEL=0` is the release level. The adapter logs decisions, state changes and errors only, and the console prefs are written false. `MOZ_LOG` in the `env` file and the `wayland-debug` marker work at either level.

## Build requirements

Building Firefox takes a lot of disk space, memory and time. Everything runs in a Linux container (`ffbuild`, podman), whose `/work` volume holds the source, toolchains and build output. Measured on an Apple Silicon Mac with a podman VM of 12 CPUs, 48 GB RAM and a 140 GB disk:

| | Disk |
|---|---|
| Firefox source (`/work/firefox`) | 5.6 GB |
| Firefox build output, per toolchain (`/work/obj-arm32`, `/work/obj-nc4`) | 3.7 GB each |
| Rust toolchain and cargo | 2.5 GB |
| Debian armel sysroot (Debian build only) | 0.7 GB |
| buildroot-nc4 SDK build, plus 1.8 GB of downloads (portable build only) | 22 GB |
| Everything, both builds | 39 GB |

Allow at least **60 GB free** for one build and 100 GB for both: build output grows beyond these finished sizes while linking.

**CPU and memory.** Firefox's build runs one compile job per core and estimates 1 GB of RAM per job, so plan on at least 16 GB of RAM (32 GB or more is comfortable); linking `libxul.so` alone needs several GB. With 12 cores:

| Step | Time |
|---|---|
| First full Firefox build, Rust included | about 1.5 to 2 hours |
| Rebuild after a change to C/C++ flags or a forced header | about 25 to 30 minutes |
| Relink after changing only link flags | a few minutes |
| buildroot-nc4 SDK with GTK (serial at the top level, as nc4 builds) | about 45 to 60 minutes |

Fewer cores scale these up roughly in proportion. The package itself is about 100 MB and the installed runtime about 250 MB on the TV.

## Portable build (buildroot-nc4, glibc 2.12)

`build/mozconfig-nc4` builds the same Firefox against the [buildroot-nc4](https://github.com/openlgtv/buildroot-nc4) SDK instead of Debian: glibc 2.12.2, GCC 16's libstdc++ (linked statically), and a GTK 3 stack added by `build/nc4/firefox.fragment`. Every shipped file, bundled libraries included, needs nothing newer than glibc 2.12, so one package is meant to run on webOS 4 and later (kernel 3.17 or newer). Tested on the webOS 25 TV: GPU WebRender, codecs, menus and YouTube work as with the Debian build. Steps, inside the `ffbuild` container, after `build/linux-build.sh` has fetched and patched the Firefox source:

```sh
podman exec -u builder ffbuild bash /src/build/nc4/build-sdk.sh   # SDK with GTK, ~1 h
podman exec ffbuild env CROSS=nc4 OUT=/work/nc4/adapter bash /src/build/build-adapter.sh
podman exec -u builder ffbuild bash -c 'export RUSTUP_HOME=/work/rustup CARGO_HOME=/work/cargo PATH=/work/cargo/bin:$PATH MOZBUILD_STATE_PATH=/work/mozbuild MOZCONFIG=/src/build/mozconfig-nc4; cd /work/firefox && ./mach build && ./mach package'
podman exec ffbuild env TOOLCHAIN=nc4 bash /src/build/assemble-runtime.sh
```

`build/nc4/glibc-check.sh <dir>` lists anything that would need a newer glibc. `build/nc4/glibc-compat.h` supplies the few constants and declarations glibc 2.12's headers lack.

## Firefox ESR 153

Pinned tag: `build/FIREFOX_TAG` (`FIREFOX_153_3_0esr_RELEASE`).

Rust must be 1.90 to 1.99. rustc 1.100 adds an `x86_64-oe-linux-gnu` target, and ESR 153's configure then cannot pick the host triple (`Don't know how to translate x86_64-pc-linux-gnu for rustc`). `rustup default 1.90.0` fixes it. `build/linux-build.sh` compiles it in the Ubuntu container (`podman exec ffbuild bash /src/build/linux-build.sh`). As root it installs the host packages and builds the Debian 11 armel sysroot (`build/sysroot-armel.sh`), then as the `builder` user it installs Rust, fetches the source, applies `build/patches/` and runs `mach build` and `mach package`. `build/build-adapter.sh` builds the adapter library. `build/assemble-runtime.sh` turns `mach package` into `app/firefox-runtime`, bundling the libraries the TV lacks, and packs `dist/com.github.gprot42.geckotv_<version>_arm.ipk`. Installed, the runtime is about 253 MB. TV prefs are copied into the runtime from `app/defaults/pref/00-webos.js`. Firefox only scans `defaults/pref` and `defaults/preferences`; a file under `distribution/preferences` is never read. Sandbox environment variables are set by the launcher because the process is already inside webOS `jailer`.
