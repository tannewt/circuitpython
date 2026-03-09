#!/bin/bash
# Ensure west can discover its workspace when running from the
# bind-mounted CircuitPython tree.
#
# We symlink the container's pre-built west workspace directories into
# the port tree so west's topdir discovery works normally.
#
# Set USE_HOST_WEST=1 to skip this (e.g. if you have a local west
# workspace you want to use instead).

if [ "${USE_HOST_WEST}" = "1" ]; then
    exec "$@"
fi

ZEPHYR_WS=/opt/zephyr-workspace
PORT_DIR=/circuitpython/ports/zephyr-cp

# Symlink west-managed top-level directories into the port tree.
for dir in zephyr modules tools bootloader; do
    target="${PORT_DIR}/${dir}"
    if [ ! -e "${target}" ]; then
        ln -s "${ZEPHYR_WS}/${dir}" "${target}"
    fi
done

# Ensure .west/config points to the right manifest with relative paths.
# This works whether the host already had .west/ or not.
mkdir -p "${PORT_DIR}/.west"
cat > "${PORT_DIR}/.west/config" <<EOF
[manifest]
path = zephyr-config

[zephyr]
base = zephyr
EOF

exec "$@"
