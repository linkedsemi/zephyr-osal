if(CONFIG_ZEPHYR_OSAL)
    zephyr_library_named(zephyr-osal)

if(CONFIG_SOC_LSQSH_CPU2)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/i2c/i2c_dev.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/i2c/test.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/gpio/gpio_pub.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/gpio/gpio_dev.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/gpio/test.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/kcs/kcs_dev.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/kcs/kcs_test.c
        # ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/led/led_dev.c
        # ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/led/test.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/random/random.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/realpath.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/eeprom/eeprom_dev.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/eeprom/test.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/uart/uart_dev.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/overlayfs/overlay_fs.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/uart/uart_test.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/overlayfs/utils.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/fs/truncate.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/hwmon/hwmon.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/hwmon/hwmon_ls.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/hwmon/test.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/alibaba-misc/ali-moc-fpga.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/jtag/jtagm_fs.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/jtag/test.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/espi/espi_dev.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/espi/test.c

        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/ipmb/ipmb_dev.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/ipmb/ipmb_test.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/stub/tcsendbreak.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/shell/fs/cmd_dd.c
    )
  zephyr_library_sources_ifdef(CONFIG_BOARD_ALISMC ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/hwmon/hwmon_pmbus_status.c)
elseif(CONFIG_SOC_LSQSH_CPU1)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/realpath.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/fs/pipe.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/net/getifaddrs.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/overlayfs/overlay_fs.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/overlayfs/utils.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/gpio/gpio_pub.c
    )
elseif(CONFIG_BOARD_QEMU_RISCV32_QEMU_VIRT_RISCV32)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/realpath.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/overlayfs/overlay_fs.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/overlayfs/utils.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/gpio/gpio_pub.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/gpio/gpio_dev.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/random/random.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/fs/ramdisk/ramdisk.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/fs/truncate.c

        ${ZEPHYR_CURRENT_MODULE_DIR}/src/stub/tcsendbreak.c
    )

    zephyr_library_link_libraries(FS)
endif()

    # Always compile pipe and getifaddrs (needed regardless of CPU config)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/fs/pipe.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/net/getifaddrs.c
        )

    # IPMI request latency probe (RAM ring + "ipmi_lat" shell command), see
    # include/ipmi_lat.h. It lives in this module next to its header instead of
    # in the application so that every branch that uses zephyr-osal links the
    # probe on its own, even if it does not pick up the application's changes:
    # sdbusplus' connection.hpp, basu and dbus-broker declare ipmi_lat_cyc(),
    # ipmi_lat_bus_step(), ipmi_lat_write_retry() and the brq_*/brs_* hooks
    # themselves and would otherwise be left with undefined references.
    # Compiled unconditionally: the whole body is behind
    # CONFIG_OPENBMC_IPMI_LAT, and the timing helpers must exist even when the
    # probe is off.
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/ipmi_lat.c
        )

    zephyr_library_include_directories(
        ${ZEPHYR_CURRENT_MODULE_DIR}/include
        ${ZEPHYR_CURRENT_MODULE_DIR}/include/net
        )

    zephyr_export_include_directories(
        ${ZEPHYR_CURRENT_MODULE_DIR}/include
        ${ZEPHYR_CURRENT_MODULE_DIR}/include/net
        )

    #zephyr_library_link_libraries(ELMFAT)

    zephyr_library_include_directories_ifdef(CONFIG_ALIPSU ${ZEPHYR_BASE}/drivers/sensor/alipsu)
endif()

if(CONFIG_SHELL_MEMTESTER)
    zephyr_library_named(memtester)
    zephyr_library_sources(
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/shell/mem/memtester/memtester.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/shell/mem/memtester/tests.c
    )
    zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/zephyr)
    zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/)
endif()
