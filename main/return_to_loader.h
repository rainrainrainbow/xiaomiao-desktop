/*
 * return_to_loader.h — drop-in for xiaomao-loader (jsfaint) compatible ROM.
 *
 * Include this header and call return_to_loader_setup() as the very
 * first line of app_main().  It points the OTA boot partition back to
 * the factory (loader) partition, so that any reset, power-cycle, or
 * esp_restart() returns to xiaomao-loader instead of re-entering this
 * ROM.
 *
 * Compatible with jsfaint/xiaomiao-loader partition layout:
 *   - factory    @ 0x10000  (Loader firmware, persistent)
 *   - launcher   @ 0xA0000  (subtype=ota_0, this OS firmware)
 *   - retro-core @ 0x2C0000 (subtype=ota_1, optional second slot)
 *   - otadata    @ 0x9E000  (boot selection)
 *
 * Combined with CONFIG_BOOTLOADER_FACTORY_RESET=GPIO12, holding the
 * B button while powering on boots the loader directly.
 */

#pragma once

#include "esp_ota_ops.h"

static inline void return_to_loader_setup(void)
{
    const esp_partition_t *cur = esp_ota_get_running_partition();
    if (!cur || cur->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0)
        return;

    const esp_partition_t *fac = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (fac)
        esp_ota_set_boot_partition(fac);
}