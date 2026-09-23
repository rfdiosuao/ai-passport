idf_component_register(
    SRCS "summon_voice.c" "summon_network.c"
         "ui_font.c"
         "provisioning.c"
    INCLUDE_DIRS "."
    REQUIRES json mbedtls bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash esp_websocket_client lwip
)

# 中文字库:按 docs/development/engineering/lvgl-chinese-fonts.md 第 4 节注册,
# 用绝对路径而不是 SRCS 里的 "../" 相对路径(后者在部分构建配置下不可移植)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/passport_cjk_16.c"
)
