idf_component_register(SRCS "main.c"
                            "sensors.c"
                            "i2c_io.c"
                            "max30102.c"
                            "max30102_algorithm.c"
                            "max30205.c"
                            "bmi270_imu.c"
                            "vital_alert.c"
                    PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi esp_timer
                                  esp_driver_gpio esp_driver_i2c esp_hw_support
                                  savi_proto savi_battery
                    INCLUDE_DIRS ".")
