/* Arduino.h stub for PC-side tests.
 * Placed in tools/nanopb_test/ so -Itools/nanopb_test satisfies
 * any '#include <Arduino.h>' or '#include "Arduino.h"' in firmware headers.
 * Only provides the types that DeviceTable.h / LoRaFrame.h actually need.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* Arduino String type is defined in pc_shim.h (force-included first).
 * Nothing else needed here. */
