#include "return_to_loader.h"
#include "esp_system.h"
#include "rom/ets_sys.h"

static uint32_t s_reset_reason = 0;

uint32_t return_to_loader_get_reset_reason(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    s_reset_reason = (uint32_t)reason;
    return s_reset_reason;
}

void return_to_loader_setup(void)
{
    s_reset_reason = return_to_loader_get_reset_reason();
}
