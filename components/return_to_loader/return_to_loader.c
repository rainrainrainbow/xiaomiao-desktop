#include "return_to_loader.h"
#include "esp_ota_ops.h"
#include "esp_log.h"

static const char *TAG = "return_to_loader";

void return_to_loader_setup(void)
{
    /* Set the factory partition as the next boot partition */
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) {
        esp_err_t err = esp_ota_set_boot_partition(factory);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Return-to-loader: will boot to factory on next restart");
        } else {
            ESP_LOGW(TAG, "Failed to set factory boot partition: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "No factory partition found");
    }
}