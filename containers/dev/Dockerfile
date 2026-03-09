ARG BASE_IMAGE=localhost/cp-ci:base
FROM ${BASE_IMAGE}

# ── Additional system packages ──────────────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
    ninja-build libusb-1.0-0 libusb-1.0-0-dev libudev-dev \
    && rm -rf /var/lib/apt/lists/*

# ── Cortex-M toolchains ────────────────────────────────────────────────────

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

# ── Espressif toolchains ───────────────────────────────────────────────────

COPY ports/espressif/esp-idf/ /circuitpython/ports/espressif/esp-idf/
ENV IDF_PATH=/circuitpython/ports/espressif/esp-idf
ENV IDF_TOOLS_PATH=/opt/esp-idf-tools

RUN $IDF_PATH/install.sh && rm -rf $IDF_TOOLS_PATH/dist

# ── Zephyr toolchains ─────────────────────────────────────────────────────

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
