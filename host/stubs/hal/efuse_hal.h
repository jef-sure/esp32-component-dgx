#pragma once
/* whether the flash is encrypted: the test that includes a bus which asks says so */
#include <stdbool.h>

bool efuse_hal_flash_encryption_enabled(void);
