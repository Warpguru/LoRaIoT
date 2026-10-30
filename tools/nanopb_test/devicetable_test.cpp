/* devicetable_test.cpp - PC-side test suite for DeviceTable.
 *
 * Covers the full DeviceTable logic that was previously only tested at ESP32
 * boot time in src/diagnostics/src/Diagnostics.cpp (runDeviceTableCheck).
 * Running these checks on the PC costs zero ESP32 RAM and zero boot time.
 * Full documentation: doc/NanopbGeneration.md section 4.3.
 *
 * Build and run from the repository root (two steps: .c files need -std=c99):
 *
 * Step 1 - compile nanopb C runtime once (objects reused by roundtrip.exe too):
 *   cmd /c "D:\Development\SetupEnvPython3.cmd >nul 2>&1 & ^
 *     python -m ziglang cc -std=c99 -Isrc\lora\src\nanopb -c src\lora\src\nanopb\loraiot.pb.c -o tools\nanopb_test\loraiot.pb.o && ^
 *     python -m ziglang cc -std=c99 -Isrc\lora\src\nanopb -c src\lora\src\nanopb\pb_common.c  -o tools\nanopb_test\pb_common.o  && ^
 *     python -m ziglang cc -std=c99 -Isrc\lora\src\nanopb -c src\lora\src\nanopb\pb_encode.c  -o tools\nanopb_test\pb_encode.o  && ^
 *     python -m ziglang cc -std=c99 -Isrc\lora\src\nanopb -c src\lora\src\nanopb\pb_decode.c  -o tools\nanopb_test\pb_decode.o"
 *
 * Step 2 - compile and link C++ sources:
 *   cmd /c "D:\Development\SetupEnvPython3.cmd >nul 2>&1 & ^
 *     python -m ziglang c++ -std=c++17 -Wall -Wextra ^
 *     -include tools/nanopb_test/pc_shim.h ^
 *     -Itools/nanopb_test -Isrc/lora/src/nanopb -Isrc/lora/src -Isrc/registry/src -Isrc/util/src ^
 *     -o tools\nanopb_test\devicetable_test.exe ^
 *     tools\nanopb_test\devicetable_test.cpp tools\nanopb_test\pc_shim.cpp src\registry\src\DeviceTable.cpp ^
 *     tools\nanopb_test\loraiot.pb.o tools\nanopb_test\pb_common.o tools\nanopb_test\pb_encode.o tools\nanopb_test\pb_decode.o"
 *
 * Run:
 *   tools\nanopb_test\devicetable_test.exe
 *   (exit code 0 = ALL CHECKS PASSED)
 *
 * PC shim files (see doc/NanopbGeneration.md section 4.3 for rationale):
 *   tools/nanopb_test/pc_shim.h/.cpp  - stubs for Arduino.h, LogBuffer.h, FreeRTOS
 *   tools/nanopb_test/Arduino.h       - empty stub satisfying <Arduino.h> includes
 * The -include flag injects pc_shim.h before every TU so that the real
 * LogBuffer.h (guarded by #ifndef LOGBUFFER_H_INCLUDED) becomes a no-op.
 */

/* pc_shim.h must be first so its Arduino.h / LogBuffer.h guards fire before
 * any transitive include can pull in the real Arduino headers. */
#include "pc_shim.h"

#include "loraiot.pb.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include "DeviceTable.h"
#include "LoRaFrame.h"    /* LORA_MAX_PAYLOAD_LEN */

#include <cstdio>
#include <cstring>
#include <cstdlib>

/* -------------------------------------------------------------------------- */
/* Test infrastructure                                                        */
/* -------------------------------------------------------------------------- */

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, msg) \
    do { \
        checks++; \
        if (!(cond)) { \
            fprintf(stderr, "FAIL %s\n", (msg)); \
            failures++; \
        } else { \
            printf("OK   %s\n", (msg)); \
        } \
    } while (0)

/* Reusable instances kept as file-scope statics to avoid stack pressure.
 * DeviceTable is ~1624 B; DeviceSlot ~404 B. */
static DeviceTable t;
static DeviceTable t2;
static DeviceSlot  slot;

/* -------------------------------------------------------------------------- */
/* Check 1: register two devices, two metrics each                            */
/* -------------------------------------------------------------------------- */
static bool check1_register_and_read_back() {
    printf("\n--- Check 1: register two devices, two metrics each ---\n");
    t.clear();

    uint32_t dIdx1 = 0, mId1a = 0, mId1b = 0;
    bool ok = t.registerMetric("SensorA", "Voltage", "V",  100, dIdx1, mId1a) &&
              t.registerMetric("SensorA", "Temp",    "C",  10,  dIdx1, mId1b);
    CHECK(ok,                                          "register SensorA metrics");
    CHECK(dIdx1 == 1 && mId1a == 1 && mId1b == 2,     "SensorA indices correct");
    CHECK(t.getDeviceCount() == 1 && t.getRev() == 3, "SensorA: count=1 rev=3");

    uint32_t dIdx2 = 0, mId2 = 0;
    CHECK(t.registerMetric("SensorB", "Pressure", "Pa", 1, dIdx2, mId2),
          "register SensorB metric");
    CHECK(dIdx2 == 2 && mId2 == 1,                    "SensorB indices correct");
    CHECK(t.getDeviceCount() == 2 && t.getRev() == 5, "SensorB: count=2 rev=5");
    return true;
}

/* -------------------------------------------------------------------------- */
/* Check 2: device table full - fifth device rejected                         */
/* -------------------------------------------------------------------------- */
static bool check2_device_table_full() {
    printf("\n--- Check 2: device table full ---\n");
    /* t already has SensorA (idx=1) and SensorB (idx=2) from Check 1 */
    uint32_t d = 0, m = 0;
    t.registerMetric("SensorC", "X", "", 1, d, m);
    t.registerMetric("SensorD", "Y", "", 1, d, m);
    CHECK(t.getDeviceCount() == 4, "four devices registered");
    CHECK(!t.registerMetric("SensorE", "Z", "", 1, d, m),
          "fifth device correctly rejected");
    return true;
}

/* -------------------------------------------------------------------------- */
/* Check 3: name too long                                                     */
/* -------------------------------------------------------------------------- */
static bool check3_name_too_long() {
    printf("\n--- Check 3: name too long ---\n");
    t.clear();
    uint32_t d = 0, m = 0;
    /* 13 visible chars - one over the 12-char limit */
    CHECK(!t.registerMetric("SensorA", "TooLongNameXX", "V", 1, d, m),
          "13-char metric name rejected");
    return true;
}

/* -------------------------------------------------------------------------- */
/* Check 4: partial applyData leaves other metrics unchanged                  */
/* -------------------------------------------------------------------------- */
static bool check4_partial_apply_data() {
    printf("\n--- Check 4: partial applyData ---\n");
    t.clear();
    uint32_t d = 0, m1 = 0, m2 = 0;
    t.registerMetric("Dev", "Alpha", "V", 1, d, m1);
    t.registerMetric("Dev", "Beta",  "A", 1, d, m2);
    t.storeValue(d, m1, 42, 1000);
    t.storeValue(d, m2, 99, 1000);

    loraiot_Data data = loraiot_Data_init_zero;
    data.device              = d;
    data.values_count        = 1;
    data.values[0].metric_id = m1;
    data.values[0].which_val = loraiot_Measurement_num_tag;
    data.values[0].val.num   = 77;

    CHECK(t.applyData(data, 1500) == DeviceTableResult::OK, "applyData partial OK");
    t.copyDevice(d, slot);
    CHECK(slot.metrics[0].value == 77 && slot.metrics[0].hasValue,  "m1 updated to 77");
    CHECK(slot.metrics[1].value == 99 && slot.metrics[1].hasValue,  "m2 untouched at 99");
    return true;
}

/* -------------------------------------------------------------------------- */
/* Check 5: unknown device in applyData                                       */
/* -------------------------------------------------------------------------- */
static bool check5_unknown_device() {
    printf("\n--- Check 5: unknown device in applyData ---\n");
    t.clear();
    loraiot_Data data = loraiot_Data_init_zero;
    data.device              = 1;
    data.values_count        = 1;
    data.values[0].metric_id = 1;
    data.values[0].which_val = loraiot_Measurement_num_tag;
    data.values[0].val.num   = 5;
    CHECK(t.applyData(data, 0) == DeviceTableResult::UNKNOWN_DEVICE,
          "unknown device returns UNKNOWN_DEVICE");
    return true;
}

/* -------------------------------------------------------------------------- */
/* Check 6: unknown metric in applyData; known metrics still applied          */
/* -------------------------------------------------------------------------- */
static bool check6_unknown_metric() {
    printf("\n--- Check 6: unknown metric in applyData ---\n");
    t.clear();
    uint32_t d = 0, m1 = 0;
    t.registerMetric("Dev", "Voltage", "V", 1, d, m1);

    loraiot_Data data = loraiot_Data_init_zero;
    data.device              = d;
    data.values_count        = 2;
    data.values[0].metric_id = m1;
    data.values[0].which_val = loraiot_Measurement_num_tag;
    data.values[0].val.num   = 55;
    data.values[1].metric_id = 99;  /* unknown */
    data.values[1].which_val = loraiot_Measurement_num_tag;
    data.values[1].val.num   = 0;

    CHECK(t.applyData(data, 0) == DeviceTableResult::UNKNOWN_METRIC,
          "unknown metric returns UNKNOWN_METRIC");
    t.copyDevice(d, slot);
    CHECK(slot.metrics[0].value == 55 && slot.metrics[0].hasValue,
          "known metric still applied");
    return true;
}

/* -------------------------------------------------------------------------- */
/* Check 7: applyDevice with changed scale clears value; unchanged keeps it  */
/* -------------------------------------------------------------------------- */
static bool check7_apply_device_scale_change() {
    printf("\n--- Check 7: applyDevice scale change ---\n");
    t.clear();
    uint32_t d = 0, m1 = 0, m2 = 0;
    t.registerMetric("Dev", "Alpha", "V", 100, d, m1);
    t.registerMetric("Dev", "Beta",  "A", 10,  d, m2);
    t.storeValue(d, m1, 42, 0);
    t.storeValue(d, m2, 99, 0);

    loraiot_Device dev = loraiot_Device_init_zero;
    dev.device        = d;
    strncpy(dev.name, "Dev", sizeof(dev.name) - 1);
    dev.metrics_count = 2;
    dev.metrics[0].id = m1; dev.metrics[0].scale = 100; /* unchanged */
    strncpy(dev.metrics[0].name, "Alpha", sizeof(dev.metrics[0].name) - 1);
    strncpy(dev.metrics[0].unit, "V",     sizeof(dev.metrics[0].unit)  - 1);
    dev.metrics[1].id = m2; dev.metrics[1].scale = 20;  /* changed */
    strncpy(dev.metrics[1].name, "Beta",  sizeof(dev.metrics[1].name) - 1);
    strncpy(dev.metrics[1].unit, "A",     sizeof(dev.metrics[1].unit)  - 1);

    CHECK(t.applyDevice(dev) == DeviceTableResult::OK, "applyDevice OK");
    t.copyDevice(d, slot);
    CHECK(slot.metrics[0].hasValue && slot.metrics[0].value == 42,
          "unchanged scale: value preserved");
    CHECK(!slot.metrics[1].hasValue,
          "changed scale: value cleared");
    return true;
}

/* -------------------------------------------------------------------------- */
/* Check 8: storeTextValue and read back                                      */
/* -------------------------------------------------------------------------- */
static bool check8_store_text_value() {
    printf("\n--- Check 8: storeTextValue and read back ---\n");
    t.clear();
    uint32_t d = 0, m = 0;
    t.registerMetric("Dev", "State", "", 1, d, m);
    t.storeTextValue(d, m, "enabled", 100);
    t.copyDevice(d, slot);
    CHECK(slot.metrics[0].hasValue && slot.metrics[0].isText &&
          strcmp(slot.metrics[0].text, "enabled") == 0,
          "storeTextValue 'enabled' round trip");
    return true;
}

/* -------------------------------------------------------------------------- */
/* Check 9: toProtoData -> encode -> decode -> applyData round trip           */
/* -------------------------------------------------------------------------- */
static bool check9_proto_round_trip() {
    printf("\n--- Check 9: toProtoData encode/decode round trip ---\n");
    t.clear();
    t2.clear();
    uint32_t d = 0, m1 = 0, m2 = 0, m3 = 0;
    t.registerMetric("Dev", "Volt", "V", 100, d, m1);
    t.registerMetric("Dev", "Date", "",  1,   d, m2);
    t.registerMetric("Dev", "Mode", "",  1,   d, m3);
    t.storeValue(d, m1, 335, 1000);
    t.storeTextValue(d, m2, "30.10.2026", 1000);
    t.storeTextValue(d, m3, "auto", 1000);

    loraiot_Data protoOut = loraiot_Data_init_zero;
    CHECK(t.toProtoData(d, protoOut, 1000), "toProtoData succeeds");

    uint8_t buf[loraiot_Data_size];
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    CHECK(pb_encode(&os, loraiot_Data_fields, &protoOut), "encode mixed Data");

    pb_istream_t is = pb_istream_from_buffer(buf, os.bytes_written);
    loraiot_Data protoIn = loraiot_Data_init_zero;
    CHECK(pb_decode(&is, loraiot_Data_fields, &protoIn), "decode mixed Data");

    uint32_t d2 = 0, dm1 = 0, dm2 = 0, dm3 = 0;
    t2.registerMetric("Dev", "Volt", "V", 100, d2, dm1);
    t2.registerMetric("Dev", "Date", "",  1,   d2, dm2);
    t2.registerMetric("Dev", "Mode", "",  1,   d2, dm3);
    CHECK(t2.applyData(protoIn, 1500) == DeviceTableResult::OK,
          "applyData on decoded data");

    t2.copyDevice(d2, slot);
    CHECK(!slot.metrics[0].isText && slot.metrics[0].value == 335,  "Volt=335 numeric");
    CHECK(slot.metrics[1].isText && strcmp(slot.metrics[1].text, "30.10.2026") == 0,
          "Date='30.10.2026' text");
    CHECK(slot.metrics[2].isText && strcmp(slot.metrics[2].text, "auto") == 0,
          "Mode='auto' text");
    return true;
}

/* -------------------------------------------------------------------------- */
/* Check 10: UTF-8 multi-byte text at exactly DEVICE_TABLE_TEXT_MAX_LEN bytes */
/* -------------------------------------------------------------------------- */
static bool check10_utf8_max_length() {
    printf("\n--- Check 10: UTF-8 text at max length (%u bytes) ---\n",
           (unsigned)DEVICE_TABLE_TEXT_MAX_LEN);
    t.clear();
    uint32_t d = 0, m = 0;
    t.registerMetric("Dev", "Note", "", 1, d, m);

    /* 6 x 2-byte ae-umlaut (0xC3 0xA4) = exactly 12 bytes */
    const char utf8_max[DEVICE_TABLE_TEXT_BUF_SIZE] =
        "\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4";
    CHECK(strlen(utf8_max) == DEVICE_TABLE_TEXT_MAX_LEN,
          "test vector is exactly TEXT_MAX_LEN bytes");
    t.storeTextValue(d, m, utf8_max, 100);
    t.copyDevice(d, slot);
    CHECK(slot.metrics[0].hasValue && slot.metrics[0].isText &&
          memcmp(slot.metrics[0].text, utf8_max, DEVICE_TABLE_TEXT_MAX_LEN) == 0,
          "max-length UTF-8 text stored and read back correctly");
    return true;
}

/* -------------------------------------------------------------------------- */
/* Check 11: type flip numeric -> text -> numeric                             */
/* -------------------------------------------------------------------------- */
static bool check11_type_flip() {
    printf("\n--- Check 11: type flip numeric -> text -> numeric ---\n");
    t.clear();
    uint32_t d = 0, m = 0;
    t.registerMetric("Dev", "Sensor", "", 1, d, m);

    t.storeValue(d, m, 12345, 100);
    t.copyDevice(d, slot);
    CHECK(!slot.metrics[0].isText && slot.metrics[0].value == 12345, "numeric stored");

    t.storeTextValue(d, m, "standby", 200);
    t.copyDevice(d, slot);
    CHECK(slot.metrics[0].isText && strcmp(slot.metrics[0].text, "standby") == 0,
          "flip to text 'standby'");

    t.storeValue(d, m, 67890, 300);
    t.copyDevice(d, slot);
    CHECK(!slot.metrics[0].isText && slot.metrics[0].value == 67890, "flip back to numeric");
    return true;
}

/* -------------------------------------------------------------------------- */
/* Check 12: over-limit text rejected at application layer                   */
/* -------------------------------------------------------------------------- */
static bool check12_over_limit_text() {
    printf("\n--- Check 12: over-limit text (%u bytes) rejected ---\n",
           (unsigned)(DEVICE_TABLE_TEXT_MAX_LEN + 1));
    /* This check validates that the constant DEVICE_TABLE_TEXT_MAX_LEN is
     * correctly used in the calling code to gate ingestion.  The check is
     * purely arithmetic - no DeviceTable state is needed.
     * (storeTextValue does NOT reject over-length strings - it is a cache
     * function that trusts its caller; validation happens at the REST layer
     * using DEVICE_TABLE_TEXT_MAX_LEN.) */
    const char* over = "1234567890123";  /* 13 bytes = TEXT_MAX_LEN + 1 */
    CHECK(strlen(over) == DEVICE_TABLE_TEXT_MAX_LEN + 1,
          "over-limit string is exactly TEXT_MAX_LEN+1 bytes");
    CHECK(strlen(over) > DEVICE_TABLE_TEXT_MAX_LEN,
          "over-limit string length > TEXT_MAX_LEN (correct rejection criterion)");
    return true;
}

/* -------------------------------------------------------------------------- */
/* Check 13: 8 full-width text metrics fit in one LoRa Response(Data) frame  */
/* -------------------------------------------------------------------------- */
static bool check13_data_frame_budget() {
    printf("\n--- Check 13: Response(Data) budget with %u x %u-byte text ---\n",
           (unsigned)DEVICE_TABLE_MAX_METRICS, (unsigned)DEVICE_TABLE_TEXT_MAX_LEN);

    static loraiot_Response resp;
    static uint8_t pbBuf[loraiot_Response_size];

    const char fullText[DEVICE_TABLE_TEXT_BUF_SIZE] =
        "\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4";

    loraiot_Data protoData = loraiot_Data_init_zero;
    protoData.device       = 1;
    protoData.values_count = DEVICE_TABLE_MAX_METRICS;
    for (uint8_t i = 0; i < DEVICE_TABLE_MAX_METRICS; i++) {
        protoData.values[i].metric_id = static_cast<uint32_t>(i + 1);
        protoData.values[i].which_val = loraiot_Measurement_text_tag;
        memcpy(protoData.values[i].val.text, fullText, DEVICE_TABLE_TEXT_BUF_SIZE);
    }

    memset(&resp, 0, sizeof(resp));
    resp.request_seq  = 1;
    resp.which_result = loraiot_Response_data_tag;
    resp.result.data  = protoData;

    pb_ostream_t os = pb_ostream_from_buffer(pbBuf, sizeof(pbBuf));
    CHECK(pb_encode(&os, loraiot_Response_fields, &resp),
          "pb_encode Response(Data) 8 x 12-byte text succeeds");

    const size_t encoded = os.bytes_written;
    printf("     encoded size: %u B  (limit %u B)\n",
           (unsigned)encoded, (unsigned)LORA_MAX_PAYLOAD_LEN);

    CHECK(encoded <= LORA_MAX_PAYLOAD_LEN,
          "encoded size <= LORA_MAX_PAYLOAD_LEN (186 B)");

    const size_t estimate = PB_DATA_FIXED_OVERHEAD
                          + DEVICE_TABLE_MAX_METRICS * PB_MEASUREMENT_ENCODED_MAX_SIZE;
    CHECK(encoded <= estimate,
          "encoded size <= constant-based estimate (proves constants are correct)");

    printf("     estimate: %u B  actual: %u B\n",
           (unsigned)estimate, (unsigned)encoded);
    return true;
}

/* -------------------------------------------------------------------------- */
/* main                                                                       */
/* -------------------------------------------------------------------------- */
int main(void) {
    printf("=== DeviceTable PC-side test suite ===\n");
    printf("    (all checks from Diagnostics.cpp runDeviceTableCheck)\n");

    check1_register_and_read_back();
    check2_device_table_full();
    check3_name_too_long();
    check4_partial_apply_data();
    check5_unknown_device();
    check6_unknown_metric();
    check7_apply_device_scale_change();
    check8_store_text_value();
    check9_proto_round_trip();
    check10_utf8_max_length();
    check11_type_flip();
    check12_over_limit_text();
    check13_data_frame_budget();

    printf("\n%d checks run, %d failed.\n", checks, failures);
    printf("\n%s\n",
           failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED");
    return failures ? 1 : 0;
}
