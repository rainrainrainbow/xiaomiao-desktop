#ifndef RETURN_TO_LOADER_H
#define RETURN_TO_LOADER_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t return_to_loader_get_reset_reason(void);
void return_to_loader_setup(void);

#ifdef __cplusplus
}
#endif

#endif // RETURN_TO_LOADER_H
