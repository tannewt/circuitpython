# CircuitPython Container Build Plan

## Goals

1. **Faster CI** — Eliminate repeated toolchain/dependency installation by baking them into container images.
2. **Easier local builds** — `docker run` should "just work" without manual toolchain setup.
3. **Better Codespaces** — Faster startup using pre-built images instead of post-create scripts.
4. **Shared source layer** — Build a per-commit source layer in the scheduler job so downstream board builds have zero setup.

## Container Image Layering

```
ubuntu:24.04
  └── circuitpython-base
  │     Python 3, requirements-dev.txt, CMake, mtools, dosfstools, uncrustify
  │
  ├── circuitpython-cortex-m  (CI image for most ports)
  │     + ARM GCC 15.2.Rel1 (arm-none-eabi)
  │     + AArch64 GCC 15.2.Rel1 (aarch64-none-elf, for broadcom)
  │     + RISC-V GCC 8.3.0 (riscv64-unknown-elf, for litex)
  │     + nrfutil (for nordic)
  │     + all submodules
  │
  ├── circuitpython-espressif  (CI image for espressif port)
  │     + ESP-IDF (from ports/espressif/esp-idf submodule)
  │     + All Xtensa and RISC-V ESP toolchains (via idf install.sh)
  │     + ninja-build
  │     + all submodules
  │
  ├── circuitpython-zephyr  (CI image for zephyr-cp port)
  │     + Zephyr SDK (arm-zephyr-eabi toolchain)
  │     + west + west update (pins from zephyr-config/west.yml)
  │     + libusb-1.0-0-dev, libudev-dev, mtools
  │     + all submodules
  │
  └── circuitpython-dev  (devcontainer/Codespaces image)
        + ALL toolchains from above (cortex-m + espressif + zephyr)
        + all submodules
```

### Layer design rationale

- **CI images are toolchain-specific** to minimize pull size for each job.
- **Dev image includes everything** so any board can be built from one container.
- **Submodules are a late layer** in each image. They rarely change, so Docker layer
  caching keeps rebuilds fast. When they do change, only the submodule layer rebuilds.
- **litex folds into cortex-m** since it just adds one more GCC variant.
- **broadcom extras (aarch64 GCC, mkfs.fat) fold into cortex-m** similarly.

## Container Rebuild Triggers

Each image rebuilds only when its inputs change:

| Image | Rebuild when these change |
|-------|--------------------------|
| **base** | `containers/base/Dockerfile`, `requirements-dev.txt` |
| **cortex-m** | above + `containers/cortex-m/Dockerfile`, submodule refs |
| **espressif** | above + `containers/espressif/Dockerfile`, submodule refs, `ports/espressif/esp-idf` commit |
| **zephyr** | above + `containers/zephyr/Dockerfile`, submodule refs, `ports/zephyr-cp/zephyr-config/west.yml` |
| **dev** | any of the above |

Triggered by: push to main with relevant path changes, or manual `workflow_dispatch`.

No scheduled rebuilds. If nothing changed, nothing rebuilds.

## CI Workflow Architecture

### Current flow (per board build)

```
checkout → setup python → fetch port deps → fetch submodules →
install python deps → download mpy-cross → build board
```

Each board build repeats all setup steps independently (~2-4 min each).

### New flow

```
Container rebuild workflow (on-demand, only when inputs change):
  Build and push: base, cortex-m, espressif, zephyr, dev images

Scheduler job (per CI run):
  1. Determine build matrix (same as today)
  2. Build mpy-cross
  3. For each needed port image:
     - Build a source overlay layer on top of the toolchain image
       (COPY source into the container; submodules already present)
     - Push as ghcr.io/adafruit/circuitpython-ci:<port>-<sha>
     Only the thin source layer is new; heavy layers are cached.

Board build job:
  container: ghcr.io/adafruit/circuitpython-ci:cortex-m-<sha>
  steps:
    - make BOARD=xxx   ← source, toolchain, and deps are all in the container

Cleanup job (after all builds):
  - Delete ephemeral <port>-<sha> tags from registry
```

### Port-to-image mapping

| Port | CI Image |
|------|----------|
| atmel-samd | cortex-m |
| raspberrypi | cortex-m |
| mimxrt10xx | cortex-m |
| stm | cortex-m |
| nordic | cortex-m |
| broadcom | cortex-m |
| cxd56 | cortex-m |
| silabs | cortex-m |
| analog | cortex-m |
| litex | cortex-m |
| espressif | espressif |
| zephyr-cp | zephyr |
| unix | base |

## Dockerfile Sketches

### `containers/base/Dockerfile`

```dockerfile
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    python3 python3-pip python3-venv \
    git cmake mtools uncrustify \
    gettext wget curl \
    && rm -rf /var/lib/apt/lists/*

# dosfstools 4.2 (newer than apt version)
RUN wget https://github.com/dosfstools/dosfstools/releases/download/v4.2/dosfstools-4.2.tar.gz \
    && tar xf dosfstools-4.2.tar.gz \
    && cd dosfstools-4.2 && ./configure && make && make install \
    && cd .. && rm -rf dosfstools-4.2*

COPY requirements-dev.txt /tmp/
RUN pip install --break-system-packages -r /tmp/requirements-dev.txt

WORKDIR /circuitpython
```

### `containers/cortex-m/Dockerfile`

```dockerfile
FROM ghcr.io/adafruit/circuitpython-ci:base

# ARM GCC 15.2.Rel1
RUN wget -qO- "https://developer.arm.com/-/media/Files/downloads/gnu/15.2.rel1/binrel/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi.tar.xz" \
    | tar xJ -C /opt/
ENV PATH="/opt/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin:${PATH}"

# AArch64 GCC (for broadcom port)
RUN wget -qO- "https://adafruit-circuit-python.s3.amazonaws.com/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf.tar.xz" \
    | tar xJ -C /opt/
ENV PATH="/opt/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-elf/bin:${PATH}"

# RISC-V GCC (for litex port)
RUN wget -qO- "https://static.dev.sifive.com/dev-tools/riscv64-unknown-elf-gcc-8.3.0-2019.08.0-x86_64-linux-centos6.tar.gz" \
    | tar xz -C /opt/
ENV PATH="/opt/riscv64-unknown-elf-gcc-8.3.0-2019.08.0-x86_64-linux-centos6/bin:${PATH}"

# nrfutil (for nordic port)
RUN wget -qO /usr/local/bin/nrfutil "https://developer.nordicsemi.com/.pc-tools/nrfutil/x64-linux/nrfutil" \
    && chmod +x /usr/local/bin/nrfutil \
    && nrfutil install nrf5sdk-tools

# Submodules (late layer — rarely changes)
COPY lib/ /circuitpython/lib/
COPY extmod/ulab/ /circuitpython/extmod/ulab/
COPY tools/ /circuitpython/tools/
```

### `containers/espressif/Dockerfile`

```dockerfile
FROM ghcr.io/adafruit/circuitpython-ci:base

RUN apt-get update && apt-get install -y \
    ninja-build libusb-1.0-0 \
    && rm -rf /var/lib/apt/lists/*

# ESP-IDF
COPY ports/espressif/esp-idf/ /circuitpython/ports/espressif/esp-idf/
ENV IDF_PATH=/circuitpython/ports/espressif/esp-idf
ENV IDF_TOOLS_PATH=/opt/esp-idf-tools

RUN $IDF_PATH/install.sh && rm -rf $IDF_TOOLS_PATH/dist

# Submodules (late layer)
COPY lib/ /circuitpython/lib/
COPY extmod/ulab/ /circuitpython/extmod/ulab/
COPY tools/ /circuitpython/tools/
```

### `containers/zephyr/Dockerfile`

```dockerfile
FROM ghcr.io/adafruit/circuitpython-ci:base

RUN apt-get update && apt-get install -y \
    ninja-build libusb-1.0-0-dev libudev-dev mtools \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --break-system-packages west

# Zephyr SDK
RUN wget -qO- "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.0/zephyr-sdk-0.17.0_linux-x86_64.tar.xz" \
    | tar xJ -C /opt/ \
    && /opt/zephyr-sdk-0.17.0/setup.sh -t arm-zephyr-eabi
ENV ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-0.17.0

# West workspace — pins come from zephyr-config/west.yml
COPY ports/zephyr-cp/zephyr-config/west.yml /circuitpython/ports/zephyr-cp/zephyr-config/west.yml
WORKDIR /circuitpython/ports/zephyr-cp
RUN west init -l zephyr-config && west update
RUN west zephyr-export
WORKDIR /circuitpython

# Submodules (late layer)
COPY lib/ /circuitpython/lib/
COPY extmod/ulab/ /circuitpython/extmod/ulab/
COPY tools/ /circuitpython/tools/
```

### `containers/dev/Dockerfile`

```dockerfile
FROM ghcr.io/adafruit/circuitpython-ci:base

# Includes ALL toolchains so any board can be built.
# (Installs cortex-m + espressif + zephyr toolchains)
# ...
```

### `containers/source-overlay/Dockerfile`

```dockerfile
ARG BASE_IMAGE
FROM ${BASE_IMAGE}

# Overlay current source; submodules already present in base
COPY . /circuitpython/
```

Built per CI run:
```bash
docker build \
  --build-arg BASE_IMAGE=ghcr.io/adafruit/circuitpython-ci:cortex-m \
  -t ghcr.io/adafruit/circuitpython-ci:cortex-m-$SHA \
  -f containers/source-overlay/Dockerfile .
```

## Devcontainer Updates

Replace the per-port configs with a single devcontainer using the dev image:

```json
{
  "image": "ghcr.io/adafruit/circuitpython-ci:dev",
  "postCreateCommand": ".devcontainer/post_create.sh"
}
```

`post_create.sh` shrinks to:
```bash
#!/bin/bash
make fetch-tags
make -C mpy-cross -j$(nproc)
```

Startup drops from ~10 min to ~1 min.

## Testing Locally

All examples use `docker`; substitute `podman` if needed. For podman rootless mode,
ensure `/etc/subuid` and `/etc/subgid` are configured:

```bash
sudo usermod --add-subuids 100000-165535 --add-subgids 100000-165535 $USER
podman system migrate
```

### Build the images

The cortex-m Dockerfile uses a `BASE_IMAGE` build arg that defaults to `localhost/cp-ci:base`
for local builds. In CI this would be overridden to the registry path.

```bash
# Build base image (uses containers/base/.dockerignore to send only requirements-dev.txt)
docker build -t cp-ci:base -f containers/base/Dockerfile .

# Build cortex-m on top
docker build -t cp-ci:cortex-m -f containers/cortex-m/Dockerfile .
```

### Verify toolchains

```bash
docker run --rm cp-ci:cortex-m bash -c "\
  arm-none-eabi-gcc --version | head -1 && \
  aarch64-none-elf-gcc --version | head -1 && \
  riscv64-unknown-elf-gcc --version | head -1 && \
  nrfutil --version && \
  python3 --version && \
  cmake --version | head -1"
```

### Interactive local development (volume mount)

Mount your source tree into the container. The git repo is present so version
detection works normally.

```bash
docker run --rm -it -v $(pwd):/circuitpython cp-ci:cortex-m bash
# Inside:
make -C mpy-cross -j$(nproc)
cd ports/atmel-samd && make BOARD=feather_m4_express -j$(nproc)
```

### Test the source overlay (simulates CI)

The `.dockerignore` at the repo root excludes `.git` and `build-*` directories.
Since there is no git repo in the image, set `CP_VERSION` to bypass git version
detection.

```bash
# Build source overlay
docker build \
  --build-arg BASE_IMAGE=cp-ci:cortex-m \
  -t cp-ci:cortex-m-test \
  -f containers/source-overlay/Dockerfile .

# Build a board — no volume mount needed, source is baked in
docker run --rm -e CP_VERSION=10.1.2 cp-ci:cortex-m-test \
  bash -c "make -C mpy-cross -j$(nproc) && \
           cd ports/atmel-samd && \
           make BOARD=feather_m4_express -j$(nproc)"
```

Note: `py/version.py` will emit noisy `git diff` help text to stderr when no git
repo is present. This is cosmetic — the build completes successfully. We should
fix that script to suppress stderr or check for a git repo before calling `git diff`.

### Test devcontainer in VS Code

```
Command Palette → "Dev Containers: Reopen in Container"
```

Should start in ~1 min with all tools ready.

## Verified Test Results

The following have been tested locally with podman 5.8.0:

- [x] Base image builds successfully (ubuntu:24.04 + Python 3.12 + CMake 3.28 + dosfstools + pip deps)
- [x] Cortex-m image builds successfully (ARM GCC 15.2.Rel1 + AArch64 GCC + RISC-V GCC + nrfutil)
- [x] Volume-mount build: `feather_m4_express` firmware.uf2 produced successfully (with CP_VERSION env)
- [x] Source overlay build: `feather_m4_express` firmware.uf2 produced successfully (with CP_VERSION env)
- [ ] Espressif image build (not yet tested)
- [ ] Zephyr image build (not yet tested)
- [ ] Dev image build (not yet tested)

## Implementation Order

### Phase 1: Dockerfiles and container build workflow
1. [x] Create `containers/base/Dockerfile`
2. [x] Create `containers/cortex-m/Dockerfile`
3. [x] Create `containers/espressif/Dockerfile`
4. [x] Create `containers/zephyr/Dockerfile`
5. [x] Create `containers/dev/Dockerfile`
6. [x] Create `containers/source-overlay/Dockerfile`
7. [x] Create `.github/workflows/build-containers.yml`
8. Test all images locally

### Phase 2: Switch CI to containers
9. [x] Update `build-boards.yml` to use container images (dual-path: container + bare)
10. [x] Update `build.yml` to pass container-image per port
11. Add cleanup job to delete ephemeral images after CI run
12. Update `run-tests.yml` to use container images
13. Remove or simplify `.github/actions/deps/external/` and port-specific dep actions

### Phase 3: Devcontainer modernization
14. Update `.devcontainer/` to use the dev image
15. Simplify `post_create.sh`
16. Remove per-port devcontainer configs (single config for all ports)
17. Remove old setup scripts

### Phase 4: Documentation and cleanup
18. Update `BUILDING.md` with container-based build instructions
19. Add convenience Makefile targets (`make docker-build BOARD=xxx`)
20. Remove dead code from old setup flow

## Open Questions

- **Registry naming**: `ghcr.io/adafruit/circuitpython-ci` — confirm org/repo with Adafruit.
- **ESP-IDF size**: ESP-IDF is ~2GB with tools. May want multi-stage builds to trim.
- **mpy-cross**: Currently built per-run because it encodes the CP version. Could we pass
  the version at build time instead? If so, mpy-cross could go into the image.
- **Ephemeral image cleanup**: GHCR doesn't have TTL for tags. Need a cleanup step or
  script to delete old `<port>-<sha>` images after CI runs.
- **ARM64 runners**: If GitHub adds ARM64 runners, we'd need multi-arch images via
  `docker buildx`.
