#ifndef RETURN_TO_LOADER_H
#define RETURN_TO_LOADER_H

#include "esp_err.h"

/**
 * @brief Setup return-to-loader mechanism.
 * Must be called as the first line in app_main().
 * When the app exits, it will return to the loader (factory partition).
 */
void return_to_loader_setup(void);

#endif /* RETURN_TO_LOADER_H */