#include "display.h"
#include "transport.h"
#include "esp_log.h"

void app_main(void)
{
    display_init();
    esp_err_t err = transport_start();
    if (err != ESP_OK) {
        ESP_LOGE("app", "System start failed: %s", esp_err_to_name(err));
    }
}
