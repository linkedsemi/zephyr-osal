if(CONFIG_ZEPHYR_OSAL)
    zephyr_library()
    zephyr_library_sources(
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/i2c/i2c_dev.c
            ${ZEPHYR_CURRENT_MODULE_DIR}/src/driver/i2c/test.c
    )
    zephyr_library_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/)

    zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)

endif()
