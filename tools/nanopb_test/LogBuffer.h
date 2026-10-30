/* LogBuffer.h stub for PC-side tests.
 * Placed in tools/nanopb_test/ but DeviceTable.cpp includes it via a
 * relative path "../../util/src/LogBuffer.h".  The -include pc_shim.h
 * compiler flag fires this stub's guards before that include is reached,
 * making the real LogBuffer.h a no-op.  The Log_* macros are defined in
 * pc_shim.h which is force-included first.
 */
#pragma once
/* All content is already provided by pc_shim.h (force-included via -include).
 * This file exists only so that any stray direct #include "LogBuffer.h"
 * resolves without error when -Itools/nanopb_test is on the path. */
#include "pc_shim.h"
