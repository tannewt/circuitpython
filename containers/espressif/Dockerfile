ARG BASE_IMAGE=localhost/cp-ci:base
FROM ${BASE_IMAGE}

RUN apt-get update && apt-get install -y --no-install-recommends \
    ninja-build libusb-1.0-0 \
    && rm -rf /var/lib/apt/lists/*

# ESP-IDF
COPY ports/espressif/esp-idf/ /circuitpython/ports/espressif/esp-idf/
ENV IDF_PATH=/circuitpython/ports/espressif/esp-idf
ENV IDF_TOOLS_PATH=/opt/esp-idf-tools

RUN $IDF_PATH/install.sh && rm -rf $IDF_TOOLS_PATH/dist
