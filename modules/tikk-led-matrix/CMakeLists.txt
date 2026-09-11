if (CONFIG_TIKK_LED_MATRIX)
zephyr_library()
zephyr_include_directories(include)

configure_file("${ZEPHYR_BASE}/../modules/lib/pixel_font/font5x8.h" "font5x8.h" COPYONLY)
zephyr_include_directories(${CMAKE_CURRENT_BINARY_DIR})

zephyr_library_sources(tikk_led_matrix.c)
endif (CONFIG_TIKK_LED_MATRIX)
