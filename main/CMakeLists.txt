set(app_srcs
    "main.c"
    "config.c"
    "core_dump.c"
    "log.c"
    "status_led.c"
    "stream_stats.c"
    "uart.c"
    "protocol/nmea.c"
)

set(app_requires
    app_update
    board_config
    bootloader
    bootloader_support
    duty_cycle
    driver
    esp_eth
    esp_bootloader_format
    esp_event
    esp_http_server
    esp_https_ota
    esp_netif
    esp_partition
    esp_psram
    esp_timer
    esp_wifi
    espcoredump
    json
    log
    lora_radio
    lora_transport
    lwip
    mbedtls
    nvs_flash
    rover_lora_pipeline
    rtcm_filter
    rtk_lora_pipeline
    soc
    spiffs
    tcp_transport
    vfs
    w5500
)

if(CONFIG_RTK_DEVICE_ROLE_BASE OR CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG)
    list(APPEND app_srcs
        "capabilities.c"
        "memory_policy.c"
        "ntrip_slots.c"
        "ntrip_runtime.c"
        "receiver.c"
        "interface/ntrip_util.c"
        "retry.c"
        "util.c"
        "web_server.c"
        "wifi.c"
        "network_ethernet.c"
        "network_state.c"
        "ethernet/eth_w5500.c"
        "ethernet/eth_lan8720.c"
        "interface/ntrip_server.c"
        "interface/ntrip_client.c"
        "interface/ntrip_server_2.c"
        "interface/socket_client.c"
        "interface/ntrip_caster.c"
        "interface/socket_server.c"
        "captive_portal.c"
        "network.c"
    )
endif()

idf_component_register(
    SRCS ${app_srcs}
    INCLUDE_DIRS "include" "." "ethernet"
    REQUIRES ${app_requires}
)

if(CONFIG_RTK_DEVICE_ROLE_BASE OR CONFIG_RTK_DEVICE_ROLE_DUAL_DEBUG)
spiffs_create_partition_image(www ../www FLASH_IN_PROJECT)
endif()
