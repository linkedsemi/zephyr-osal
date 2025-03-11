if(CONFIG_ZEPHYR_OSAL)
    zephyr_library()
    # zephyr_library_sources(
    # )
    zephyr_include_directories(${ZEPHYR_CURRENT_MODULE_DIR}/include)

endif()
