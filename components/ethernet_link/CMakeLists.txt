idf_component_register(SRCS "ethernet_link.c" INCLUDE_DIRS "include" REQUIRES esp_eth esp_netif esp_event esp_driver_spi esp_driver_gpio esp_hw_support esp_timer lwip link_common)
