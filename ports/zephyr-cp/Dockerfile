# Dockerfile for CircuitPython Zephyr port build environment.
#
# Bakes in the Zephyr SDK, toolchains, and west workspace so CI jobs
# and local devcontainer builds don't need to fetch them every time.
#
# Build args let the CI workflow pin exact versions. To rebuild locally:
#   docker build -t ghcr.io/adafruit/circuitpython-zephyr:latest -f ports/zephyr-cp/Dockerfile .

FROM ubuntu:25.10

ARG WEST_MANIFEST_DIR=zephyr-config

ENV DEBIAN_FRONTEND=noninteractive

# System dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ccache \
    cmake \
    curl \
    device-tree-compiler \
    dfu-util \
    file \
    gcc-multilib \
    g++-multilib \
    git \
    gperf \
    libmagic1 \
    libsdl2-dev \
    libusb-1.0-0-dev \
    libudev-dev \
    make \
    mtools \
    ninja-build \
    protobuf-compiler \
    python3 \
    python3-dev \
    python3-pip \
    python3-venv \
    wget \
    xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Create and activate a venv for all Python deps
ENV VIRTUAL_ENV=/opt/zephyr-venv
RUN python3 -m venv ${VIRTUAL_ENV}
ENV PATH="${VIRTUAL_ENV}/bin:${PATH}"

# Install west
RUN pip install west

# Set up the Zephyr workspace.
# Copy only the west manifest first so Docker can cache the (large) west
# update layer independently of CircuitPython source changes.
WORKDIR /opt/zephyr-workspace
COPY ports/zephyr-cp/zephyr-config/ zephyr-config/
RUN west init -l ${WEST_MANIFEST_DIR} \
    && west update --narrow -o=--depth=1 \
    && west zephyr-export

# Install Zephyr's own Python requirements
RUN pip install -r zephyr/scripts/requirements.txt protobuf grpcio-tools

# Install CircuitPython build requirements
COPY requirements-dev.txt /tmp/requirements-dev.txt
RUN pip install -r /tmp/requirements-dev.txt && rm /tmp/requirements-dev.txt

# Install the Zephyr SDK to a well-known path.
# We set HOME=/opt so west sdk install places it at /opt/zephyr-sdk-<version>,
# then symlink to /opt/zephyr-sdk.  setup.sh records its install path
# internally, so we must not move the directory after installation.
ENV HOME=/opt
RUN west sdk install \
    && ln -s /opt/zephyr-sdk-* /opt/zephyr-sdk \
    && /opt/zephyr-sdk/setup.sh -c
ENV HOME=/root

# Point CMake and west at the pre-built Zephyr workspace.
# ZEPHYR_BASE is used by west and many Zephyr scripts.
# ZEPHYR_SDK_INSTALL_DIR tells FindZephyr-sdk.cmake where the SDK lives.
# CMAKE_PREFIX_PATH lets find_package(Zephyr) work without relying on the
# per-user CMake package registry (~/.cmake/packages), which won't exist
# when CI runs the container as a different UID than the build-time user.
ENV ZEPHYR_BASE=/opt/zephyr-workspace/zephyr
ENV ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk
ENV CMAKE_PREFIX_PATH=/opt/zephyr-workspace/zephyr/share/zephyr-package:/opt/zephyr-workspace/zephyr/share/sysbuild-package

# Entrypoint ensures west can find its workspace from the bind-mounted
# CircuitPython tree by symlinking .west if it doesn't already exist.
COPY ports/zephyr-cp/entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]

WORKDIR /circuitpython/ports/zephyr-cp

CMD ["/bin/bash"]
