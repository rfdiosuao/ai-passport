idf_component_register(
    SRCS "main.c"
         "demo_navigation.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "ui_text.c"
         "ui_font.c"
         "provisioning.c"
         "demo_display.c"
         "demo_button.c"
         "demo_audio.c"
         "demo_battery.c"
         "demo_radio.c"
         "demo_wifi.c"
         "demo_ble.c"
         "demo_low_power.c"
         "demo_provisioning.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_http_server esp_netif esp_timer esp_wifi nvs_flash
)

# 中文字库:按 docs/development/engineering/lvgl-chinese-fonts.md 第 4 节注册,
# 用绝对路径而不是 SRCS 里的 "../" 相对路径(后者在部分构建配置下不可移植)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/passport_cjk_16.c"
)
