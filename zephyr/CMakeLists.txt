if(CONFIG_ZEPHYR_OSAL)
    zephyr_library_named(zephyr-osal)
if(CONFIG_BOARD_ALIMOC30_CMU610_CPU2)
    zephyr_library_sources(

            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/i2c/i2c_dev.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/i2c/test.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/gpio/gpio_pub.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/gpio/gpio_dev.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/gpio/test.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/kcs/kcs_dev.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/kcs/kcs_test.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/led/led_dev.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/led/test.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/random/random.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/realpath.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/eeprom/eeprom_dev.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/eeprom/test.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/uart/uart_dev.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/overlayfs/overlay_fs.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/overlayfs/utils.c
    )
else()
    zephyr_library_sources(
        ${ZEPHYR_CURRENT_MODULE_DIR}/src/realpath.c
    )
endif()
    zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/)

    zephyr_export_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)

    zephyr_library_link_libraries(ELMFAT)
endif()
