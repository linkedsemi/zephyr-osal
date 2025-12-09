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

        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/ipmb/ipmb_dev.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/ipmb/ipmb_test.c
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/stub/tcsendbreak.c
    )
  zephyr_library_sources_ifdef(CONFIG_BOARD_ALISMC ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/hwmon/hwmon_pmbus_status.c)
elseif(CONFIG_SOC_LSQSH_CPU1)
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/realpath.c
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

    zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/)

    zephyr_export_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)

    zephyr_library_link_libraries(ELMFAT)

    zephyr_library_include_directories_ifdef(CONFIG_ALIPSU ${ZEPHYR_BASE}/drivers/sensor/alipsu)
endif()
