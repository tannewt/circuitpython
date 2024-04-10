$(BUILD)/lib/picolibc/thumbv6m/newlib/libc.a:
	mkdir -p $(BUILD)/lib/picolibc/thumbv6m/
	meson setup --reconfigure -Dtests=false -Dmultilib=false --cross-file picolibc/scripts/cross-clang-thumbv6m-none-eabi.txt picolibc $(BUILD)/lib/picolibc/thumbv6m/
	ninja -j 4

$(BUILD)/lib/picolibc/thumbv6m/newlib/libm.a: $(BUILD)/lib/picolibc/thumbv6m/newlib/libc.a
