import sys
import pathlib
import tempfile
from types import SimpleNamespace

# Add devicetree library to path
portdir = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(portdir / "zephyr/scripts/dts/python-devicetree/src/"))

from devicetree import dtlib

# Add parent directory to path to import zephyr2cp
sys.path.insert(0, str(pathlib.Path(__file__).parent.parent))

# Mock cpbuild before importing
sys.modules["cpbuild"] = type(sys)("cpbuild")
sys.modules["cpbuild"].run_in_thread = lambda x: x

import pytest

from zephyr2cp import (
    find_flash_devices,
    find_ram_regions,
    BLOCKED_FLASH_COMPAT,
    MINIMUM_RAM_SIZE,
    STEMMA_I2C_PINS,
    add_toml_pin_names,
    _board_i2c_buses,
    _fastest_first,
    _named_pins,
    _standard_bus_pins,
    _toml_status_dotstar,
    _toml_status_pin,
    _used_by_zephyr,
)


def parse_dts_string(dts_content):
    """
    Parse a device tree string and return the dtlib.DT object.

    Args:
        dts_content: String containing device tree source

    Returns:
        dtlib.DT object with parsed device tree
    """
    with tempfile.NamedTemporaryFile(mode="w", suffix=".dts", delete=False) as f:
        f.write(dts_content)
        f.flush()
        temp_path = f.name

    try:
        dt = dtlib.DT(temp_path)
        return dt
    finally:
        pathlib.Path(temp_path).unlink()


class TestFindFlashDevices:
    """Test suite for find_flash_devices function."""

    def test_no_compatible_returns_empty(self):
        """Test that device tree with no flash devices returns empty list."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;

    memory@0 {
        reg = <0x0 0x100000>;
    };

    chosen {
    };
};
"""
        dt = parse_dts_string(dts)
        result = find_flash_devices(dt)
        assert result == []

    def test_chosen_flash_excluded(self):
        """Test that chosen flash nodes are excluded."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;

    flash0: flash@0 {
        compatible = "soc-nv-flash";
        reg = <0x0 0x100000>;
    };

    chosen {
        zephyr,flash = &flash0;
    };
};
"""
        dt = parse_dts_string(dts)
        result = find_flash_devices(dt)
        assert result == [], "Chosen flash should be excluded"

    def test_blocked_compat_excluded(self):
        """Test that blocked compatible strings are excluded."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;

    qspi0: qspi@40000 {
        compatible = "renesas,ra-qspi";
        reg = <0x40000 0x1000>;
    };

    spi0: spi@50000 {
        compatible = "nordic,nrf-spim";
        reg = <0x50000 0x1000>;
    };

    chosen {
    };
};
"""
        dt = parse_dts_string(dts)
        result = find_flash_devices(dt)
        assert result == [], "Blocked flash controllers should be excluded"

    def test_valid_flash_device(self):
        """Test that valid flash device is detected."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;

    flash0: flash@0 {
        compatible = "jedec,spi-nor";
        reg = <0x0 0x100000>;
    };

    chosen {
    };
};
"""
        dt = parse_dts_string(dts)
        result = find_flash_devices(dt)
        assert len(result) == 1
        assert result[0] == "flash0"

    def test_valid_flash_device_multiple_drivers(self):
        """Test that valid flash device is detected."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;

    flash0: flash@0 {
        compatible = "other,driver", "jedec,spi-nor";
        reg = <0x0 0x100000>;
    };

    chosen {
    };
};
"""
        dt = parse_dts_string(dts)
        result = find_flash_devices(dt)
        assert len(result) == 1
        assert result[0] == "flash0"

    def test_external_flash_not_chosen(self):
        """Test external flash is included when internal is chosen."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;

    internal_flash: flash@0 {
        compatible = "soc-nv-flash";
        reg = <0x0 0x100000>;
    };

    external_flash: flash@1000000 {
        compatible = "jedec,spi-nor";
        reg = <0x1000000 0x800000>;
    };

    chosen {
        zephyr,flash = &internal_flash;
    };
};
"""
        dt = parse_dts_string(dts)
        result = find_flash_devices(dt)

        # Should only include external flash, not chosen internal flash
        assert len(result) == 1
        assert "external_flash" in result[0]
        assert "internal_flash" not in result[0]

    def test_disabled_flash_excluded(self):
        """Test that disabled flash devices are excluded."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;

    flash0: flash@0 {
        compatible = "jedec,spi-nor";
        reg = <0x0 0x100000>;
        status = "disabled";
    };

    chosen {
    };
};
"""
        dt = parse_dts_string(dts)
        result = find_flash_devices(dt)
        assert result == [], "Disabled flash should be excluded"


class TestFindRAMRegions:
    """Test suite for find_ram_regions function."""

    def test_no_ram_returns_empty(self):
        """Test that device tree with no RAM returns empty list."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;

    flash@0 {
        compatible = "soc-nv-flash";
        reg = <0x0 0x100000>;
    };

    chosen {
    };
};
"""
        dt = parse_dts_string(dts)
        result = find_ram_regions(dt)
        assert result == []

    def test_chosen_sram_basic(self):
        """Test chosen sram region is detected correctly."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;

    sram0: memory@20000000 {
        compatible = "mmio-sram";
        reg = <0x20000000 0x40000>;
    };

    chosen {
        zephyr,sram = &sram0;
    };
};
"""
        dt = parse_dts_string(dts)
        result = find_ram_regions(dt)

        assert len(result) == 1
        label, start, end, size, path = result[0]

        assert label == "sram0"
        assert start == "z_mapped_end"
        assert (
            end
            == "(uint32_t*) (DT_REG_ADDR(DT_NODELABEL(sram0)) + DT_REG_SIZE(DT_NODELABEL(sram0)))"
        )
        assert size == 0x40000

    def test_memory_region_with_custom_name(self):
        """Test memory region with zephyr,memory-region property."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;

    sram0: memory@20000000 {
        compatible = "mmio-sram";
        reg = <0x20000000 0x40000>;
    };

    reserved_mem: memory@30000000 {
        compatible = "zephyr,memory-region", "mmio-sram";
        reg = <0x30000000 0x10000>;
        zephyr,memory-region = "CUSTOM_REGION";
    };

    chosen {
        zephyr,sram = &sram0;
    };
};
"""
        dt = parse_dts_string(dts)
        result = find_ram_regions(dt)

        # Should have both regions, chosen first
        assert len(result) == 2

        # First should be chosen SRAM
        assert result[0][0] == "sram0"

        # Second should be custom region
        label, start, end, size, path = result[1]
        assert label == "reserved_mem"
        assert start == "__CUSTOM_REGION_end"

    def test_memory_region_requires_sram_or_device_type(self):
        """Test memory regions require mmio-sram compatibility or device_type=memory."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;

    sram0: memory@20000000 {
        compatible = "mmio-sram";
        reg = <0x20000000 0x40000>;
    };

    reserved_mem: memory@30000000 {
        compatible = "zephyr,memory-region";
        reg = <0x30000000 0x10000>;
        zephyr,memory-region = "CUSTOM_REGION";
    };

    external_mem: memory@40000000 {
        compatible = "zephyr,memory-region";
        device_type = "memory";
        reg = <0x40000000 0x20000>;
        zephyr,memory-region = "EXT_REGION";
    };

    chosen {
        zephyr,sram = &sram0;
    };
};
"""
        dt = parse_dts_string(dts)
        result = find_ram_regions(dt)

        assert len(result) == 2
        assert result[0][0] == "sram0"
        assert result[1][0] == "external_mem"

    def test_disabled_ram_excluded(self):
        """Test that disabled RAM regions are excluded."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;

    sram0: memory@20000000 {
        compatible = "mmio-sram";
        reg = <0x20000000 0x40000>;
    };

    sram1: memory@30000000 {
        compatible = "zephyr,memory-region", "mmio-sram";
        reg = <0x30000000 0x10000>;
        status = "disabled";
        zephyr,memory-region = "CUSTOM_REGION";
    };

    chosen {
        zephyr,sram = &sram0;
    };
};
"""
        dt = parse_dts_string(dts)
        result = find_ram_regions(dt)

        # Should only have chosen SRAM, not disabled one
        assert len(result) == 1
        assert result[0][0] == "sram0"


class TestIntegration:
    """Integration tests with realistic device tree configurations."""

    def test_typical_nrf_board_configuration(self):
        """Test typical Nordic nRF board with internal and external flash."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;

    soc {
        #address-cells = <1>;
        #size-cells = <1>;

        flash0: flash@0 {
            compatible = "soc-nv-flash";
            reg = <0x0 0x100000>;
        };

        sram0: memory@20000000 {
            compatible = "mmio-sram";
            reg = <0x20000000 0x40000>;
        };
    };

    external_flash: spi_flash@0 {
        compatible = "jedec,spi-nor";
        reg = <0x0 0x800000>;
    };

    chosen {
        zephyr,flash = &flash0;
        zephyr,sram = &sram0;
    };
};
"""
        dt = parse_dts_string(dts)

        # Test flash detection
        flashes = find_flash_devices(dt)
        assert len(flashes) == 1, "Should find external flash only"
        assert "external_flash" in flashes[0]

        # Test RAM detection
        rams = find_ram_regions(dt)
        assert len(rams) == 1, "Should find chosen SRAM only"
        assert rams[0][0] == "sram0"

    def test_board_with_nrf5340_regions(self):
        """Test that RAM subregions are included with the right addresses."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;
    chosen {
        zephyr,sram = &sram0_image;
    };
    /* node '/soc/peripheral@50000000/qspi@2b000' defined in zephyr/dts/arm/nordic/nrf5340_cpuapp_peripherals.dtsi:475 */
			qspi: qspi@2b000 {
				compatible = "nordic,nrf-qspi";  /* in zephyr/dts/arm/nordic/nrf5340_cpuapp_peripherals.dtsi:476 */
				#address-cells = < 0x1 >;        /* in zephyr/dts/arm/nordic/nrf5340_cpuapp_peripherals.dtsi:477 */
				#size-cells = < 0x0 >;           /* in zephyr/dts/arm/nordic/nrf5340_cpuapp_peripherals.dtsi:478 */
				reg = < 0x2b000 0x1000 >,
				      < 0x10000000 0x10000000 >; /* in zephyr/dts/arm/nordic/nrf5340_cpuapp_peripherals.dtsi:479 */
				reg-names = "qspi",
				            "qspi_mm";           /* in zephyr/dts/arm/nordic/nrf5340_cpuapp_peripherals.dtsi:480 */
				interrupts = < 0x2b 0x1 >;       /* in zephyr/dts/arm/nordic/nrf5340_cpuapp_peripherals.dtsi:481 */
				status = "okay";                 /* in zephyr/boards/nordic/nrf5340dk/nrf5340_cpuapp_common.dtsi:118 */

				/* node '/soc/peripheral@50000000/qspi@2b000/mx25r6435f@0' defined in zephyr/boards/nordic/nrf5340dk/nrf5340_cpuapp_common.dtsi:123 */
				mx25r64: mx25r6435f@0 {
					compatible = "nordic,qspi-nor";                                                               /* in zephyr/boards/nordic/nrf5340dk/nrf5340_cpuapp_common.dtsi:124 */
					reg = < 0x0 >;                                                                                /* in zephyr/boards/nordic/nrf5340dk/nrf5340_cpuapp_common.dtsi:125 */
					writeoc = "pp4io";                                                                            /* in zephyr/boards/nordic/nrf5340dk/nrf5340_cpuapp_common.dtsi:127 */
					readoc = "read4io";                                                                           /* in zephyr/boards/nordic/nrf5340dk/nrf5340_cpuapp_common.dtsi:129 */
					sck-frequency = < 0x7a1200 >;                                                                 /* in zephyr/boards/nordic/nrf5340dk/nrf5340_cpuapp_common.dtsi:130 */
					jedec-id = [ C2 28 17 ];                                                                      /* in zephyr/boards/nordic/nrf5340dk/nrf5340_cpuapp_common.dtsi:131 */
					sfdp-bfp = [ E5 20 F1 FF FF FF FF 03 44 EB 08 6B 08 3B 04 BB EE FF FF FF FF FF 00 FF FF FF 00
					             FF 0C 20 0F 52 10 D8 00 FF 23 72 F5 00 82 ED 04 CC 44 83 68 44 30 B0 30 B0 F7 C4
					             D5 5C 00 BE 29 FF F0 D0 FF FF ];                                                 /* in zephyr/boards/nordic/nrf5340dk/nrf5340_cpuapp_common.dtsi:132 */
					size = < 0x4000000 >;                                                                         /* in zephyr/boards/nordic/nrf5340dk/nrf5340_cpuapp_common.dtsi:136 */
					has-dpd;                                                                                      /* in zephyr/boards/nordic/nrf5340dk/nrf5340_cpuapp_common.dtsi:137 */
					t-enter-dpd = < 0x2710 >;                                                                     /* in zephyr/boards/nordic/nrf5340dk/nrf5340_cpuapp_common.dtsi:138 */
					t-exit-dpd = < 0x88b8 >;                                                                      /* in zephyr/boards/nordic/nrf5340dk/nrf5340_cpuapp_common.dtsi:139 */
				};
			};

    		/* node '/soc/memory@20000000' defined in zephyr/dts/arm/nordic/nrf5340_cpuapp.dtsi:55 */
		sram0: memory@20000000 {
			compatible = "mmio-sram";            /* in zephyr/dts/arm/nordic/nrf5340_cpuapp.dtsi:56 */
			#address-cells = < 0x1 >;            /* in zephyr/dts/arm/nordic/nrf5340_cpuapp.dtsi:57 */
			#size-cells = < 0x1 >;               /* in zephyr/dts/arm/nordic/nrf5340_cpuapp.dtsi:58 */
			reg = < 0x20000000 0x80000 >;        /* in zephyr/dts/arm/nordic/nrf5340_cpuapp_qkaa.dtsi:15 */
			ranges = < 0x0 0x20000000 0x80000 >; /* in zephyr/dts/arm/nordic/nrf5340_cpuapp_qkaa.dtsi:16 */

			/* node '/soc/memory@20000000/sram@0' defined in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:16 */
			sram0_image: sram@0 {
				reg = < 0x0 0x70000 >;        /* in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:18 */
				ranges = < 0x0 0x0 0x70000 >; /* in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:19 */
				#address-cells = < 0x1 >;     /* in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:20 */
				#size-cells = < 0x1 >;        /* in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:21 */

				/* node '/soc/memory@20000000/sram@0/sram0_image@0' defined in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:23 */
				sram0_s: sram0_image@0 {
					reg = < 0x0 0x40000 >; /* in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:25 */
				};
			};

			/* node '/soc/memory@20000000/sram@40000' defined in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:33 */
			sram0_ns: sram@40000 {
				reg = < 0x40000 0x40000 >;        /* in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:35 */
				ranges = < 0x0 0x40000 0x40000 >; /* in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:36 */
				#address-cells = < 0x1 >;         /* in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:37 */
				#size-cells = < 0x1 >;            /* in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:38 */

				/* node '/soc/memory@20000000/sram@40000/sram0_ns@0' defined in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:40 */
				sram0_ns_app: sram0_ns@0 {
					reg = < 0x0 0x30000 >; /* in zephyr/dts/vendor/nordic/nrf5340_sram_partition.dtsi:42 */
				};
			};

			/* node '/soc/memory@20000000/sram@70000' defined in zephyr/dts/vendor/nordic/nrf5340_shared_sram_partition.dtsi:27 */
			sram0_shared: sram@70000 {
				reg = < 0x70000 0x10000 >; /* in zephyr/dts/vendor/nordic/nrf5340_shared_sram_partition.dtsi:29 */
				phandle = < 0x11 >;        /* in zephyr/dts/arm/nordic/nrf5340_cpuapp_ipc.dtsi:9 */
			};
		};
};
"""
        dt = parse_dts_string(dts)
        flashes = find_flash_devices(dt)
        rams = find_ram_regions(dt)

        # Should only get chosen SRAM
        assert len(rams) == 1
        assert rams[0][0] == "sram0_image"

        assert len(flashes) == 1
        assert flashes[0] == "mx25r64"

    def test_board_with_chosen_memory_region(self):
        """Test that RAM subregions are included with the right addresses."""
        dts = """
/dts-v1/;

/ {
    #address-cells = <1>;
    #size-cells = <1>;
    chosen {
    zephyr,sram = &axisram2;     /* in zephyr/boards/st/nucleo_n657x0_q/nucleo_n657x0_q_common.dtsi:18 */
    };


	/* node '/memory@34000000' defined in zephyr/dts/arm/st/n6/stm32n6.dtsi:42 */
	axisram1: memory@34000000 {
		compatible = "zephyr,memory-region",
		             "mmio-sram";            /* in zephyr/dts/arm/st/n6/stm32n6.dtsi:43 */
		zephyr,memory-region = "AXISRAM1";   /* in zephyr/dts/arm/st/n6/stm32n6.dtsi:44 */
		reg = < 0x34000000 0x200000 >;       /* in zephyr/dts/arm/st/n6/stm32n657X0.dtsi:12 */
	};

	/* node '/memory@34180400' defined in zephyr/dts/arm/st/n6/stm32n6.dtsi:47 */
	axisram2: memory@34180400 {
		compatible = "zephyr,memory-region",
		             "mmio-sram";            /* in zephyr/dts/arm/st/n6/stm32n6.dtsi:48 */
		zephyr,memory-region = "AXISRAM2";   /* in zephyr/dts/arm/st/n6/stm32n6.dtsi:49 */
		reg = < 0x34180400 0x7fc00 >;        /* in zephyr/dts/arm/st/n6/stm32n657X0.dtsi:17 */
	};

	/* node '/soc/ramcfg@42023100' defined in zephyr/dts/arm/st/n6/stm32n6.dtsi:251 */
	ramcfg_sram3_axi: ramcfg@42023100 {
		compatible = "st,stm32n6-ramcfg"; /* in zephyr/dts/arm/st/n6/stm32n6.dtsi:252 */
		#address-cells = < 0x1 >;         /* in zephyr/dts/arm/st/n6/stm32n6.dtsi:253 */
		#size-cells = < 0x1 >;            /* in zephyr/dts/arm/st/n6/stm32n6.dtsi:254 */
		reg = < 0x42023100 0x80 >;        /* in zephyr/dts/arm/st/n6/stm32n6.dtsi:255 */

		/* node '/soc/ramcfg@42023100/memory@34200000' defined in zephyr/dts/arm/st/n6/stm32n6.dtsi:259 */
		axisram3: memory@34200000 {
			compatible = "zephyr,memory-region",
			             "mmio-sram";            /* in zephyr/dts/arm/st/n6/stm32n6.dtsi:260 */
			zephyr,memory-region = "AXISRAM3";   /* in zephyr/dts/arm/st/n6/stm32n6.dtsi:261 */
			zephyr,memory-attr = < 0x100000 >;   /* in zephyr/dts/arm/st/n6/stm32n6.dtsi:262 */
			reg = < 0x34200000 0x70000 >;        /* in zephyr/dts/arm/st/n6/stm32n657X0.dtsi:23 */
			status = "disabled";                 /* in zephyr/dts/arm/st/n6/stm32n657X0.dtsi:24 */
		};
	};
};
"""
        dt = parse_dts_string(dts)
        rams = find_ram_regions(dt)

        print(rams)

        # Should only get chosen SRAM
        assert len(rams) == 2
        assert rams[0][0] == "axisram2"
        assert rams[1][0] == "axisram1"


class TestAddTomlPinNames:
    """Test suite for add_toml_pin_names."""

    def _add(
        self, toml_pins, package_pins, port_indexes, ioports, pad_names=None, board_names=None
    ):
        board_names = dict(board_names or {})
        pad_pin = {}
        pad_gpio = {}
        pad_names = {} if pad_names is None else dict(pad_names)
        for entry in package_pins or []:
            if "pad" not in entry and "pad_name" not in entry:
                # Informational module pin.
                continue
            pad = entry.get("pad", entry["pin"])
            pad_pin[pad] = entry["pin"]
            if "gpio" in entry:
                pad_gpio[pad] = entry["gpio"]
            if "pad_name" in entry and pad not in pad_names:
                pad_names[pad] = entry["pad_name"]
        analog_board_names = add_toml_pin_names(
            board_names,
            {"pins": toml_pins},
            package_pins,
            "nrf54l15_qfn48",
            port_indexes,
            ioports,
            pad_names,
            pad_gpio,
        )
        return board_names, analog_board_names

    def test_package_pin_number(self):
        package_pins = [
            {"pin": 1, "pad_name": "P1.00", "gpio": 32},
            {"pin": 5, "pad_name": "P0.07", "gpio": 7},
        ]
        board_names, _ = self._add(
            {"LED": 5}, package_pins, {"gpio0": 0, "gpio1": 1}, {"gpio0": {7}}
        )
        assert board_names == {("gpio0", 7): ["LED"]}

    def test_ball_id(self):
        package_pins = [{"pin": 12, "pad_name": "P1.15", "ball": "B2", "gpio": 47}]
        board_names, _ = self._add(
            {"LED": "b2"}, package_pins, {"gpio0": 0, "gpio1": 1}, {"gpio1": {15}}
        )
        assert board_names == {("gpio1", 15): ["LED"]}

    def test_name_sanitized(self):
        package_pins = [{"pin": 3, "pad_name": "P0.00", "gpio": 0}]
        board_names, _ = self._add({"boot button": 3}, package_pins, {"gpio0": 0}, {"gpio0": {0}})
        assert board_names == {("gpio0", 0): ["BOOT_BUTTON"]}

    def test_missing_package_map_raises(self):
        with pytest.raises(RuntimeError, match="package pin map"):
            add_toml_pin_names(
                {},
                {"pins": {"LED": 1}},
                None,
                "custom",
                {"gpio0": 0},
                {"gpio0": {0}},
                {},
                {},
            )

    def test_unknown_package_pin_raises(self):
        package_pins = [{"pin": 1, "pad_name": "P1.00", "gpio": 32}]
        with pytest.raises(RuntimeError, match="package pin 9 is not in"):
            self._add({"LED": 9}, package_pins, {"gpio0": 0, "gpio1": 1}, {"gpio0": {0}})

    def test_unknown_ball_raises(self):
        package_pins = [{"pin": 1, "pad_name": "P1.00", "gpio": 32}]
        with pytest.raises(RuntimeError, match="ball Z9"):
            self._add({"LED": "Z9"}, package_pins, {"gpio0": 0, "gpio1": 1}, {"gpio0": {0}})

    def test_bad_value_type_raises(self):
        package_pins = [{"pin": 1, "pad_name": "P1.00", "gpio": 32}]
        with pytest.raises(RuntimeError, match="package pin number"):
            self._add({"LED": True}, package_pins, {"gpio0": 0, "gpio1": 1}, {"gpio0": {0}})

    def test_bad_name_raises(self):
        package_pins = [{"pin": 1, "pad_name": "P1.00", "gpio": 32}]
        with pytest.raises(RuntimeError, match="not usable"):
            self._add({"1 LED": 1}, package_pins, {"gpio0": 0, "gpio1": 1}, {"gpio0": {0}})

    def test_pad_not_on_gpio_controller_raises(self):
        # A pad bonded to a GPIO controller the board doesn't enable: it is
        # an error.
        package_pins = [{"pin": 1, "pad_name": "P1.00", "gpio": 32}]
        with pytest.raises(RuntimeError, match="not on an enabled GPIO controller"):
            self._add({"LED": 1}, package_pins, {"gpio0": 0, "gpio1": 1}, {"gpio0": {0}})

    def test_analog_only_pad_returns_names_by_pad(self):
        # Pads whose package map entry carries no GPIO bond (analog-only
        # pads like MCX N's ANA pads) return their board module names keyed
        # by SoC pad; the caller creates their pin objects from the map's
        # datasheet pad names.
        package_pins = [{"pin": 110, "pad_name": "ANA_0"}]
        board_names, analog_board_names = self._add(
            {"A0": 110}, package_pins, {"gpio0": 0}, {"gpio0": {0}}, {110: "ANA_0"}
        )
        assert board_names == {}
        assert analog_board_names == {110: ["A0"]}

    def test_analog_pad_conflict_raises(self):
        # The name is already bound to a GPIO pad by the devicetree walk,
        # so [pins] cannot rebind it to an analog-only pad.
        package_pins = [{"pin": 110, "pad_name": "ANA_0"}]
        with pytest.raises(RuntimeError, match="already maps to gpio0 pin 0"):
            self._add(
                {"A0": 110},
                package_pins,
                {"gpio0": 0},
                {"gpio0": {0}},
                {110: "ANA_0"},
                board_names={("gpio0", 0): ["A0"]},
            )

    def test_no_pins_is_noop(self):
        board_names = {}
        analog = add_toml_pin_names(
            board_names, {}, None, None, {"gpio0": 0}, {"gpio0": {0}}, {}, {}
        )
        assert board_names == {}
        assert analog == {}


class TestAddTomlPinNamesDuplicates:
    """Deduplication and conflict behavior of add_toml_pin_names."""

    PACKAGE_PINS = [
        {"pin": 5, "pad_name": "P1.04", "gpio": 36},
        {"pin": 27, "pad_name": "P0.02", "gpio": 2},
    ]
    PORT_INDEXES = {"gpio0": 0, "gpio1": 1}
    IOPORTS = {"gpio0": {2}, "gpio1": {4}}

    def _add(self, toml_pins, board_names=None):
        board_names = dict(board_names or {})
        add_toml_pin_names(
            board_names,
            {"pins": toml_pins},
            self.PACKAGE_PINS,
            "nrf54l15_qfn48",
            self.PORT_INDEXES,
            self.IOPORTS,
            {e["pin"]: e["pad_name"] for e in self.PACKAGE_PINS},
            {e["pin"]: e["gpio"] for e in self.PACKAGE_PINS},
        )
        return board_names

    def test_dedupes_two_keys_same_name_same_pin(self):
        board_names = self._add({"MY PIN": 5, "my-pin": 5})
        assert board_names == {("gpio1", 4): ["MY_PIN"]}

    def test_conflict_two_keys_same_name_different_pins(self):
        with pytest.raises(RuntimeError, match="already maps to gpio1 pin 4"):
            self._add({"MY PIN": 5, "my-pin": 27})

    def test_dedupes_existing_devicetree_name_same_pin(self):
        board_names = self._add({"green led": 5}, {("gpio1", 4): ["Green LED"]})
        assert board_names == {("gpio1", 4): ["Green LED"]}

    def test_conflict_existing_devicetree_name_different_pin(self):
        with pytest.raises(RuntimeError, match="already maps to gpio0 pin 2"):
            self._add({"STATUS": 5}, {("gpio0", 2): ["STATUS"]})


class TestTomlStatusPin:
    """Resolution of circuitpython.toml STATUS_NEOPIXEL-style keys to pins."""

    BOARD_NAMES = {
        ("gpio0", 16): ["NEOPIXEL"],
        ("gpio1", 14): ["NEOPIXEL_POWER"],
        ("gpio1", 15): ["LED", "L"],
    }

    def test_resolves_pins_table_name(self):
        assert _toml_status_pin(
            {"STATUS_NEOPIXEL": "NEOPIXEL"}, self.BOARD_NAMES, "STATUS_NEOPIXEL"
        ) == ("gpio0", 16)

    def test_matches_sanitized_name(self):
        assert _toml_status_pin(
            {"STATUS_NEOPIXEL_POWER": "neopixel-power"},
            self.BOARD_NAMES,
            "STATUS_NEOPIXEL_POWER",
        ) == ("gpio1", 14)

    def test_absent_key_is_none(self):
        assert _toml_status_pin({}, self.BOARD_NAMES, "STATUS_NEOPIXEL") is None
        assert _toml_status_pin(None, self.BOARD_NAMES, "STATUS_NEOPIXEL") is None

    def test_unknown_name_raises(self):
        with pytest.raises(RuntimeError, match="STATUS_NEOPIXEL = 'NOPE' is not a board pin name"):
            _toml_status_pin({"STATUS_NEOPIXEL": "NOPE"}, self.BOARD_NAMES, "STATUS_NEOPIXEL")


class TestStandardBusPins:
    """The standard pins of board.I2C(), board.SPI() and board.UART()."""

    BOARD_NAMES = {
        ("gpio0", 11): ["SCL"],
        ("gpio0", 12): ["SDA"],
        ("gpio0", 14): ["SCK"],
        ("gpio0", 13): ["MOSI"],
        ("gpio0", 15): ["MISO"],
        ("gpio0", 25): ["TX"],
        ("gpio0", 24): ["RX"],
    }

    def test_all_standard_names(self):
        assert _standard_bus_pins(self.BOARD_NAMES) == {
            "i2c": {"scl": ("gpio0", 11), "sda": ("gpio0", 12)},
            "spi": {"clock": ("gpio0", 14), "mosi": ("gpio0", 13), "miso": ("gpio0", 15)},
            "uart": {"tx": ("gpio0", 25), "rx": ("gpio0", 24)},
        }

    def test_missing_name_leaves_out_bus(self):
        names = {key: value for key, value in self.BOARD_NAMES.items() if value != ["MISO"]}
        assert list(_standard_bus_pins(names)) == ["i2c", "uart"]

    def test_no_standard_names(self):
        assert _standard_bus_pins({("gpio0", 2): ["D2"]}) == {}

    def test_pin_names_ignore_case(self):
        names = dict(self.BOARD_NAMES)
        names[("gpio0", 11)] = ["scl"]
        assert _standard_bus_pins(names)["i2c"]["scl"] == ("gpio0", 11)

    def test_connector_aliases(self):
        # Arduino header pins carry both their header name and the standard name.
        names = {
            ("gpio1", 0): ["D0", "RX"],
            ("gpio1", 1): ["D1", "TX"],
            ("gpio1", 2): ["D14", "SDA"],
            ("gpio1", 3): ["D15", "SCL"],
        }
        assert _standard_bus_pins(names) == {
            "i2c": {"scl": ("gpio1", 3), "sda": ("gpio1", 2)},
            "uart": {"tx": ("gpio1", 1), "rx": ("gpio1", 0)},
        }


class TestUsedByZephyr:
    """Fixed bus instances that Zephyr itself uses."""

    DTS = """
/dts-v1/;
/ {
    chosen {
        zephyr,console = &uart0;
    };
    uart0: uart@0 {
    };
    uart1: uart@1 {
    };
    spi0: spi@0 {
        flash@0 {
            reg = <0>;
        };
    };
    spi1: spi@1 {
    };
    spi2: spi@2 {
    };
    display {
        spi-dev = <&spi1>;
    };
    disabled_display {
        status = "disabled";
        spi-dev = <&spi2>;
    };
};
"""

    def _used(self, label):
        dt = parse_dts_string(self.DTS)
        return _used_by_zephyr(dt, dt.label2node[label])

    def test_chosen_console(self):
        assert self._used("uart0")

    def test_child_device(self):
        assert self._used("spi0")

    def test_referenced_by_phandle(self):
        assert self._used("spi1")

    def test_referenced_only_by_disabled_node(self):
        assert not self._used("spi2")

    def test_unused(self):
        assert not self._used("uart1")


class TestStemmaI2cPins:
    """The pins of board.STEMMA_I2C, named by the stemma-qt-connector entry."""

    def test_same_pins_as_i2c(self):
        names = {("gpio0", 3): ["SCL", "STEMMA_SCL"], ("gpio0", 2): ["SDA", "STEMMA_SDA"]}
        assert _named_pins(names, STEMMA_I2C_PINS) == {
            "scl": ("gpio0", 3),
            "sda": ("gpio0", 2),
        }

    def test_own_pins(self):
        names = {
            ("gpio0", 25): ["SCL"],
            ("gpio0", 24): ["SDA"],
            ("gpio0", 23): ["SCL1", "STEMMA_SCL"],
            ("gpio0", 22): ["SDA1", "STEMMA_SDA"],
        }
        assert _named_pins(names, STEMMA_I2C_PINS) == {
            "scl": ("gpio0", 23),
            "sda": ("gpio0", 22),
        }

    def test_no_connector(self):
        assert _named_pins({("gpio0", 3): ["SCL"], ("gpio0", 2): ["SDA"]}, STEMMA_I2C_PINS) is None


class TestBoardI2cBuses:
    """The board I2C buses for board.I2C() and board.STEMMA_I2C."""

    I2C = {"scl": ("gpio0", 25), "sda": ("gpio0", 24)}
    STEMMA = {"scl": ("gpio0", 23), "sda": ("gpio0", 22)}

    def test_no_stemma_connector(self):
        assert _board_i2c_buses(self.I2C, None) == ([self.I2C], None)

    def test_stemma_on_the_i2c_pins(self):
        assert _board_i2c_buses(self.I2C, dict(self.I2C)) == ([self.I2C], 0)

    def test_stemma_on_other_pins(self):
        assert _board_i2c_buses(self.I2C, self.STEMMA) == ([self.I2C, self.STEMMA], 1)

    def test_stemma_without_standard_i2c(self):
        assert _board_i2c_buses(None, self.STEMMA) == ([self.STEMMA], 0)

    def test_neither(self):
        assert _board_i2c_buses(None, None) == ([], None)


class TestFastestFirst:
    """Ordering of bus instance pools by devicetree max-frequency."""

    @staticmethod
    def _node(name, max_frequency=None):
        props = {}
        if max_frequency is not None:
            props["max-frequency"] = SimpleNamespace(to_num=lambda: max_frequency)
        return SimpleNamespace(name=name, props=props)

    def test_highest_first(self):
        nodes = [self._node("spi2", 8_000_000), self._node("spi3", 32_000_000)]
        assert [n.name for n in _fastest_first(nodes)] == ["spi3", "spi2"]

    def test_stable_for_equal_and_missing(self):
        nodes = [
            self._node("i2c1"),
            self._node("spi22", 8_000_000),
            self._node("i2c0"),
            self._node("spi21", 8_000_000),
            self._node("spi00", 32_000_000),
        ]
        assert [n.name for n in _fastest_first(nodes)] == [
            "spi00",
            "spi22",
            "spi21",
            "i2c1",
            "i2c0",
        ]


class TestTomlStatusDotstar:
    """Resolution of circuitpython.toml STATUS_DOTSTAR_DATA/CLOCK keys."""

    BOARD_NAMES = {
        ("gpio0", 8): ["DOTSTAR_DATA"],
        ("gpio1", 9): ["DOTSTAR_CLOCK"],
        ("gpio0", 16): ["NEOPIXEL"],
    }
    DOTSTAR = {"STATUS_DOTSTAR_DATA": "DOTSTAR_DATA", "STATUS_DOTSTAR_CLOCK": "DOTSTAR_CLOCK"}

    def test_resolves_data_and_clock(self):
        assert _toml_status_dotstar(self.DOTSTAR, self.BOARD_NAMES) == (
            ("gpio0", 8),
            ("gpio1", 9),
        )

    def test_absent_keys_are_none(self):
        assert _toml_status_dotstar({}, self.BOARD_NAMES) is None
        assert _toml_status_dotstar(None, self.BOARD_NAMES) is None

    def test_one_key_only_raises(self):
        with pytest.raises(RuntimeError, match="needs both"):
            _toml_status_dotstar({"STATUS_DOTSTAR_DATA": "DOTSTAR_DATA"}, self.BOARD_NAMES)

    def test_with_status_neopixel_raises(self):
        with pytest.raises(RuntimeError, match="both STATUS_NEOPIXEL and a status DotStar"):
            _toml_status_dotstar({**self.DOTSTAR, "STATUS_NEOPIXEL": "NEOPIXEL"}, self.BOARD_NAMES)
