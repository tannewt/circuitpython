ARG BASE_IMAGE=localhost/cp-ci:base
FROM ${BASE_IMAGE}

RUN apt-get update && apt-get install -y --no-install-recommends \
    ninja-build libusb-1.0-0-dev libudev-dev mtools \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --break-system-packages west

# West workspace — pins come from zephyr-config/west.yml
COPY ports/zephyr-cp/zephyr-config/west.yml /circuitpython/ports/zephyr-cp/zephyr-config/west.yml
WORKDIR /circuitpython/ports/zephyr-cp
RUN west init -l zephyr-config && west update
RUN west zephyr-export

# Zephyr SDK via west
RUN west sdk install
WORKDIR /circuitpython
