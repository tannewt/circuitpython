ARG BASE_IMAGE=localhost/cp-ci:base
FROM ${BASE_IMAGE}

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
