# Resolve board aliases before sysbuild runs, so we can set up
# board-specific overlays for MCUboot.
include(${CMAKE_CURRENT_LIST_DIR}/../boards/board_aliases.cmake)

# Boards with a partition overlay get Adaboot (MCUboot) enabled.
set(partition_overlay ${CMAKE_CURRENT_LIST_DIR}/../boards/partitions/${BOARD}.dtsi)
if(EXISTS ${partition_overlay})
  # Pass the partition overlay to MCUboot so it can find
  # boot_partition and slot0_partition.
  set(mcuboot_EXTRA_DTC_OVERLAY_FILE ${partition_overlay}
      CACHE INTERNAL "MCUboot partition overlay"
  )

  # Enable MCUboot with no signatures.
  set(_sb_confs ${CMAKE_CURRENT_LIST_DIR}/adaboot.conf)
  set(sb_board_conf ${CMAKE_CURRENT_LIST_DIR}/../boards/sysbuild_${BOARD}.conf)
  if(EXISTS ${sb_board_conf})
    list(APPEND _sb_confs ${sb_board_conf})
  endif()
  set(SB_EXTRA_CONF_FILE "${_sb_confs}"
      CACHE INTERNAL "Adaboot sysbuild conf"
  )

  # Common MCUboot conf (UF2, double-tap, etc.) plus optional board-specific fragment.
  set(_mcuboot_confs ${CMAKE_CURRENT_LIST_DIR}/mcuboot.conf)
  set(mcuboot_board_conf ${CMAKE_CURRENT_LIST_DIR}/../boards/mcuboot_${BOARD}.conf)
  if(EXISTS ${mcuboot_board_conf})
    list(APPEND _mcuboot_confs ${mcuboot_board_conf})
  endif()
  set(mcuboot_EXTRA_CONF_FILE "${_mcuboot_confs}"
      CACHE INTERNAL "MCUboot conf fragments"
  )
endif()

find_package(Sysbuild REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(sysbuild LANGUAGES)
