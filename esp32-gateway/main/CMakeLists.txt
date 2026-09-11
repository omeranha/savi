idf_component_register(SRCS "main.c" "mysql_db.c"
                    PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi mbedtls
                    INCLUDE_DIRS ".")
