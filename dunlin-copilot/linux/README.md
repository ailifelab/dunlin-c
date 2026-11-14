# Dunlin Copilot for Ubuntu

The GTK 3 client uses the desktop C API in `include/dunlin/desktop.h`. It shares the macOS client's conversation, project, model, tool, permission, extension and index services. GDK selects X11 or Wayland from the desktop session. Credentials are accessed through the desktop Secret Service, such as an unlocked GNOME Keyring in the login session. Voice and video remain experimental TODOs.

## Build

Build on the Ubuntu release used by the intended machine. Install the release's WebKitGTK development package and the native build tools.

```sh
sudo apt-get install build-essential cmake pkg-config python3 perl ca-certificates zlib1g-dev libgtk-3-dev libsecret-1-dev libxml2-dev dpkg-dev
# On Bionic install libwebkit2gtk-4.0-dev; on Noble install libwebkit2gtk-4.1-dev.
```

The first build downloads and verifies the upstream SQLite amalgamation, json-c, OpenSSL and curl sources. The private desktop library links these versions so the application can use FTS5, the SQLite transaction APIs, curl URL validation and current TLS on either distribution. The build uses system GTK and Secret Service. For a build without network access, populate `CACHE` with the exact pinned archives first and set `OFFLINE=1`. Build products are written under `build/linux`; change `BUILD` to an absolute directory to keep outputs for different distributions or architectures separate.

```sh
make -C dunlin-copilot/linux deb
```

When building the optional runtime assets locally, provide Python 3.9+, Node 26 and npm 11 as described below. On stock Ubuntu 18.04, prepare those assets on a modern build host and pass `DSH_ASSETS` to make. To run the development build use `./build/linux/dunlin-copilot`. The Debian package installs the GTK client, private desktop library, application launcher, icon, context strategy, DSH assets and third-party notices. Install the package for the release and architecture with `sudo apt install ./dunlin-copilot_VERSION_ARCH_ubuntuRELEASE.deb`.

## DSH build assets and optional runtime

Prepare the locked Linux assets on a build host with Python 3.9+, Node 26 and npm 11. The script installs packages without lifecycle scripts and selects native packages for Linux/glibc and the requested CPU. This permits building the ARM64 package on a macOS build host. Use `x64` for AMD64.

```sh
python3 scripts/prepare-dsh-assets.py build/linux-assets/arm64 --linux-arch arm64
make -C dunlin-copilot/linux DSH_ASSETS="$PWD/build/linux-assets/arm64" deb
```

To reuse a prepared asset tree with an offline build, set `DSH_ASSETS` for that build as well. The installer checks its file digests and native executable formats before copying files. The default build creates the assets under `build/linux/ExtensionAssets`; the package installs them under `/usr/share/dunlin-copilot/ExtensionAssets`. The optional runtime engine remains disabled until requested by the user. The upstream Node 26.13.0 binaries require glibc 2.28. On Ubuntu 18.04, enabling the pinned Linux engine also downloads the matching Ubuntu `libc6_2.39-0ubuntu8.12` archive for AMD64 or ARM64. The client verifies the Ubuntu archive digest, extracts ten runtime objects and the copyright notice into the user's private compatibility directory, and verifies the individual objects before each launch. The Node worker and the Node children it starts use the private ELF loader. The application and other processes continue using the system libraries. Ubuntu 24.04 uses its native loader. An offline installation may place the exact archive named `libc6_2.39-0ubuntu8.12_{arm64,amd64}.deb` under the active identity's `Compatibility/dsh/` directory before enabling the engine. A corrupt archive, object or unexpected runtime file stops startup with a diagnostic.

The upstream package comes from Ubuntu's signed noble-updates index. Matching source and licensing information are available from [Ubuntu's glibc 2.39-0ubuntu8.12 release](https://launchpad.net/ubuntu/+source/glibc/2.39-0ubuntu8.12). This source tree and its application package distribute only the digest pins and loader integration.

## Display checks

Install the headless display and accessibility tools in the test environment:

```sh
sudo apt-get install xvfb xauth weston dbus-x11 at-spi2-core python3-pyatspi
make -C dunlin-copilot/linux check-backend
```

This opens the application on Xvfb and on a headless Weston compositor. Each check uses its own D-Bus session and temporary application directory. It navigates the actual GTK window through AT-SPI. Install `xserver-xorg-core xserver-xorg-video-dummy` and set `DUNLIN_XSERVER=xorg` to run the X11 check on an Xorg virtual display and the Wayland check on a nested Weston compositor with an input seat and software rendering. A desktop using the Xorg server can also run `GDK_BACKEND=x11 python3 dunlin-copilot/linux/tests/native_backend.py build/linux/dunlin-copilot`; an active Wayland desktop can use `GDK_BACKEND=wayland`.
