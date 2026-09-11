idf_component_register(SRCS "main.c"
                            "sensors.c"
                            "i2c_io.c"
                            "max30102.c"
                            "max30205.c"
                            "bmi270_imu.c"
                    PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi
                                  esp_driver_gpio esp_driver_i2c
                    INCLUDE_DIRS ".")
