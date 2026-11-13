#ifndef PATCHS_OPLUS_SKU_H
#define PATCHS_OPLUS_SKU_H

#include <stdbool.h>
#include <stdint.h>

/* Rewrite only the isolated androidboot.product.hardware.sku output for
 * project 25001; leave the shared classifier and oplusboot.sku untouched. */
bool patch_sku_25001_to_31(char* buffer, int32_t size);

#endif /* PATCHS_OPLUS_SKU_H */
