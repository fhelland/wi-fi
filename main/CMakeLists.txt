idf_component_register(SRCS "spi.c" "uart_tcp_server.c" "file_server.c" "sdmmc.c" "main.c" "wifi_manager.c" "json.c" "nvs_sync.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "webfiles/favicon.ico" "webfiles/file_manager.html" "webfiles/upgrade.html" "webfiles/wifi.html" "webfiles/logo.png" "webfiles/file.png" "webfiles/folder.png" "webfiles/back.png" "webfiles/home.png")