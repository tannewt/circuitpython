ARG BASE_IMAGE=localhost/cp-ci:base
FROM ${BASE_IMAGE}

RUN apt-get update && apt-get install -y --no-install-recommends \
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
