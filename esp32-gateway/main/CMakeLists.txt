idf_component_register(SRCS "main.c" "supabase_db.c" PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi esp_http_client mbedtls INCLUDE_DIRS ".")
