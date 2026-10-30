/* Host-side round-trip test of the generated nanopb code (no ESP32 needed).
 * Build and run: see doc/NanopbGeneration.md (compiled with "python -m ziglang cc").
 * It prints the encoded bytes so they can be compared with check_proto.py.
 *
 * Edge-case coverage (max and max+1 for every bounded field):
 *   Metric.name     max 12 chars encodes; 13-char wire payload rejected at decode
 *   Metric.unit     max  5 chars encodes; 6-char wire payload rejected at decode
 *   Device.name     max 12 chars encodes; 13-char wire payload rejected at decode
 *   Device.metrics  max  8 entries encodes; 9-entry wire payload rejected at decode
 *   Measurement.text max 12 bytes encodes; 13-byte wire payload rejected at decode
 *   Data.values     max 16 entries encodes; 17-entry wire payload rejected at decode
 *   Echo.text       max 175 chars encodes; 176-char wire payload rejected at decode
 */
#include <stdio.h>
#include <string.h>
#include "pb_encode.h"
#include "pb_decode.h"
#include "loraiot.pb.h"

/* Limits from the .options file (doc/LoRaProtocol.md section 10) */
#define PING_TEST_BOOT_ID    0x12345678u
static const uint8_t PING_EXPECTED_PAYLOAD[] = {0x08, 0xF8, 0xAC, 0xD1, 0x91, 0x01};
#define MAX_PAYLOAD_LEN      186   /* E220 sub-packet 200 B minus 14 B frame overhead */
#define METRIC_NAME_MAX      12    /* Metric.name / Device.name: 12 characters */
#define METRIC_NAME_BUF      13    /* buffer size including NUL */
#define METRIC_UNIT_MAX       5    /* Metric.unit: 5 characters */
#define METRIC_UNIT_BUF       6    /* buffer size including NUL */
#define DEVICE_METRICS_MAX    8    /* Device.metrics max_count */
#define MEAS_TEXT_MAX        12    /* Measurement.text: 12 bytes */
#define MEAS_TEXT_BUF        13    /* buffer size including NUL */
#define DATA_VALUES_MAX      16    /* Data.values max_count */
#define ECHO_TEXT_MAX       175    /* Echo.text: 175 characters */
#define ECHO_TEXT_BUF       176    /* buffer size including NUL */
#define TEST_METRIC_SCALE   100

static int failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { printf("FAIL %s\n", (msg)); failures++; } \
        else         { printf("OK   %s\n", (msg)); } \
    } while (0)

static void hex(const char *label, const uint8_t *b, size_t n) {
    printf("     %s:", label);
    for (size_t i = 0; i < n; i++) { printf(" %02X", b[i]); }
    printf("\n");
}

/* Helper: build a Metric wire-payload with a string field of exactly `len` bytes
 * in field `field_number` (tag = field_number << 3 | 2). Returns total byte count. */
static size_t make_string_field(uint8_t *out, uint8_t tag, size_t len, char fill) {
    out[0] = tag;
    out[1] = (uint8_t)len;
    memset(out + 2, (unsigned char)fill, len);
    return 2 + len;
}

int main(void) {
    /* Encode buffer sized to the largest message we ever encode in tests.
     * loraiot_Response_size (377) covers everything. */
    static uint8_t buf[loraiot_Response_size];
    pb_ostream_t os;
    pb_istream_t is;

    /* ------------------------------------------------------------------ */
    printf("=== Struct size checks ===\n");
    /* ------------------------------------------------------------------ */

    /* Verify every bounded C array is exactly the size the .options file specifies. */
    {
        loraiot_Metric    chk_metric    = loraiot_Metric_init_zero;
        loraiot_Device    chk_device    = loraiot_Device_init_zero;
        loraiot_Measurement chk_meas    = loraiot_Measurement_init_zero;
        loraiot_Echo      chk_echo      = loraiot_Echo_init_zero;
        CHECK(sizeof(chk_metric.name)     == METRIC_NAME_BUF,  "sizeof(Metric.name)      == 13");
        CHECK(sizeof(chk_metric.unit)     == METRIC_UNIT_BUF,  "sizeof(Metric.unit)      ==  6");
        CHECK(sizeof(chk_device.name)     == METRIC_NAME_BUF,  "sizeof(Device.name)      == 13");
        CHECK(sizeof(chk_device.metrics)  == DEVICE_METRICS_MAX * sizeof(loraiot_Metric),
                                                                "sizeof(Device.metrics[]) == 8 entries");
        CHECK(sizeof(chk_meas.val.text)   == MEAS_TEXT_BUF,    "sizeof(Measurement.text) == 13");
        CHECK(sizeof(chk_echo.text)       == ECHO_TEXT_BUF,    "sizeof(Echo.text)        == 176");
        {
            loraiot_Data chk_data = loraiot_Data_init_zero;
            CHECK(sizeof(chk_data.values) == DATA_VALUES_MAX * sizeof(loraiot_Measurement),
                                                                "sizeof(Data.values[])    == 16 entries");
        }
    }

    /* ------------------------------------------------------------------ */
    printf("\n=== PING / BEACON ===\n");
    /* ------------------------------------------------------------------ */

    /* PING: Beacon with only boot_id. Must equal the worked example of LoRaProtocol.md 4.2 */
    loraiot_Beacon ping = loraiot_Beacon_init_zero;
    ping.boot_id = PING_TEST_BOOT_ID;
    os = pb_ostream_from_buffer(buf, sizeof(buf));
    CHECK(pb_encode(&os, loraiot_Beacon_fields, &ping), "encode PING");
    hex("PING payload", buf, os.bytes_written);
    CHECK(os.bytes_written == sizeof(PING_EXPECTED_PAYLOAD) &&
          memcmp(buf, PING_EXPECTED_PAYLOAD, sizeof(PING_EXPECTED_PAYLOAD)) == 0,
          "PING payload equals the documented bytes");

    is = pb_istream_from_buffer(buf, os.bytes_written);
    loraiot_Beacon pingIn = loraiot_Beacon_init_zero;
    CHECK(pb_decode(&is, loraiot_Beacon_fields, &pingIn), "decode PING");
    CHECK(pingIn.boot_id == PING_TEST_BOOT_ID && !pingIn.has_registry_rev && !pingIn.has_fw_version,
          "PING decodes with absent optional fields");

    /* BEACON: optional registry_rev = 0 must be sent and decode as present */
    loraiot_Beacon bc = loraiot_Beacon_init_zero;
    bc.boot_id = 1;
    bc.has_registry_rev = true;
    bc.registry_rev = 0;
    os = pb_ostream_from_buffer(buf, sizeof(buf));
    CHECK(pb_encode(&os, loraiot_Beacon_fields, &bc), "encode BEACON");
    is = pb_istream_from_buffer(buf, os.bytes_written);
    loraiot_Beacon bcIn = loraiot_Beacon_init_zero;
    CHECK(pb_decode(&is, loraiot_Beacon_fields, &bcIn) && bcIn.has_registry_rev && bcIn.registry_rev == 0,
          "BEACON registry_rev=0 is present after decode");

    /* ------------------------------------------------------------------ */
    printf("\n=== Metric.name (max 12 chars, buf 13) ===\n");
    /* ------------------------------------------------------------------ */

    /* Max: 12-character name encodes and round-trips */
    {
        loraiot_Metric m = loraiot_Metric_init_zero;
        m.id = 1; m.scale = TEST_METRIC_SCALE;
        memset(m.name, 'n', METRIC_NAME_MAX);  /* 12 'n', NUL at [12] */
        os = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&os, loraiot_Metric_fields, &m),
              "Metric.name max (12 chars) encodes");
        is = pb_istream_from_buffer(buf, os.bytes_written);
        loraiot_Metric mIn = loraiot_Metric_init_zero;
        CHECK(pb_decode(&is, loraiot_Metric_fields, &mIn) && strlen(mIn.name) == METRIC_NAME_MAX,
              "Metric.name max (12 chars) round trip");
    }

    /* Max+1: wire payload with a 13-byte name string must be rejected at decode */
    {
        /* field 2 (name), wire type 2: tag = 0x12, length = 13, then 13 'n' bytes */
        uint8_t over[2 + 13];
        make_string_field(over, 0x12, 13, 'n');
        loraiot_Metric mOver = loraiot_Metric_init_zero;
        is = pb_istream_from_buffer(over, sizeof(over));
        CHECK(!pb_decode(&is, loraiot_Metric_fields, &mOver),
              "Metric.name max+1 (13 chars on wire) rejected at decode");
    }

    /* ------------------------------------------------------------------ */
    printf("\n=== Metric.unit (max 5 chars, buf 6) ===\n");
    /* ------------------------------------------------------------------ */

    /* Max: 5-character unit encodes and round-trips */
    {
        loraiot_Metric m = loraiot_Metric_init_zero;
        m.id = 1; m.scale = TEST_METRIC_SCALE;
        strcpy(m.name, "V");
        memset(m.unit, 'u', METRIC_UNIT_MAX);  /* 5 'u', NUL at [5] */
        os = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&os, loraiot_Metric_fields, &m),
              "Metric.unit max (5 chars) encodes");
        is = pb_istream_from_buffer(buf, os.bytes_written);
        loraiot_Metric mIn = loraiot_Metric_init_zero;
        CHECK(pb_decode(&is, loraiot_Metric_fields, &mIn) && strlen(mIn.unit) == METRIC_UNIT_MAX,
              "Metric.unit max (5 chars) round trip");
    }

    /* Max+1: wire payload with a 6-byte unit string must be rejected */
    {
        /* field 3 (unit), wire type 2: tag = 0x1A, length = 6, then 6 'u' bytes */
        uint8_t over[2 + 6];
        make_string_field(over, 0x1A, 6, 'u');
        loraiot_Metric mOver = loraiot_Metric_init_zero;
        is = pb_istream_from_buffer(over, sizeof(over));
        CHECK(!pb_decode(&is, loraiot_Metric_fields, &mOver),
              "Metric.unit max+1 (6 chars on wire) rejected at decode");
    }

    /* ------------------------------------------------------------------ */
    printf("\n=== Device.name (max 12 chars, buf 13) ===\n");
    /* ------------------------------------------------------------------ */

    /* Max: 12-character device name encodes and round-trips */
    {
        loraiot_Device d = loraiot_Device_init_zero;
        d.device = 1;
        memset(d.name, 'd', METRIC_NAME_MAX);  /* 12 'd', NUL at [12] */
        os = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&os, loraiot_Device_fields, &d),
              "Device.name max (12 chars) encodes");
        is = pb_istream_from_buffer(buf, os.bytes_written);
        loraiot_Device dIn = loraiot_Device_init_zero;
        CHECK(pb_decode(&is, loraiot_Device_fields, &dIn) && strlen(dIn.name) == METRIC_NAME_MAX,
              "Device.name max (12 chars) round trip");
    }

    /* Max+1: wire payload with a 13-byte device name must be rejected */
    {
        /* field 2 (name), wire type 2: tag = 0x12, length = 13, then 13 'd' bytes */
        uint8_t over[2 + 13];
        make_string_field(over, 0x12, 13, 'd');
        loraiot_Device dOver = loraiot_Device_init_zero;
        is = pb_istream_from_buffer(over, sizeof(over));
        CHECK(!pb_decode(&is, loraiot_Device_fields, &dOver),
              "Device.name max+1 (13 chars on wire) rejected at decode");
    }

    /* ------------------------------------------------------------------ */
    printf("\n=== Device.metrics (max 8 entries) ===\n");
    /* ------------------------------------------------------------------ */

    /* Max: Device with exactly 8 metrics encodes and round-trips */
    {
        loraiot_Device d = loraiot_Device_init_zero;
        d.device = 1;
        strcpy(d.name, "Dev");
        d.metrics_count = DEVICE_METRICS_MAX;
        for (uint32_t i = 0; i < DEVICE_METRICS_MAX; i++) {
            d.metrics[i].id    = i + 1;
            d.metrics[i].scale = TEST_METRIC_SCALE;
            /* short names so it fits in the encode buffer */
            d.metrics[i].name[0] = (char)('A' + (char)i);
            d.metrics[i].name[1] = '\0';
            strcpy(d.metrics[i].unit, "V");
        }
        os = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&os, loraiot_Device_fields, &d),
              "Device.metrics max (8 entries) encodes");
        is = pb_istream_from_buffer(buf, os.bytes_written);
        loraiot_Device dIn = loraiot_Device_init_zero;
        CHECK(pb_decode(&is, loraiot_Device_fields, &dIn) &&
              dIn.metrics_count == DEVICE_METRICS_MAX,
              "Device.metrics max (8 entries) round trip");
    }

    /* Max+1: hand-craft a Device wire payload with 9 metrics and verify it is rejected.
     * Each metric sub-message is: tag 0x1A (field 3, LEN), length varint, then metric body.
     * Minimal metric body: field 1 (id) = 0x08 0x01 (2 bytes). So each entry = 0x1A 0x02 0x08 0xNN.
     * We prepend device field (0x08 0x01) and produce 9 repeated field-3 entries. */
    {
        static uint8_t over9[2 + 9 * 4];
        size_t pos = 0;
        over9[pos++] = 0x08; over9[pos++] = 0x01;  /* device = 1 */
        for (uint8_t i = 0; i < 9; i++) {
            over9[pos++] = 0x1A;        /* field 3 (metrics), wire type 2 */
            over9[pos++] = 0x02;        /* length = 2 */
            over9[pos++] = 0x08;        /* field 1 (id), wire type 0 */
            over9[pos++] = (uint8_t)(i + 1);  /* id value */
        }
        loraiot_Device dOver = loraiot_Device_init_zero;
        is = pb_istream_from_buffer(over9, pos);
        CHECK(!pb_decode(&is, loraiot_Device_fields, &dOver),
              "Device.metrics max+1 (9 entries on wire) rejected at decode");
    }

    /* ------------------------------------------------------------------ */
    printf("\n=== Measurement.text (max 12 bytes, buf 13) ===\n");
    /* ------------------------------------------------------------------ */

    /* Max: exactly 12 bytes (6 x 2-byte UTF-8 ae-umlaut) encodes and round-trips */
    {
        loraiot_Measurement meas = loraiot_Measurement_init_zero;
        meas.metric_id = 1;
        meas.which_val = loraiot_Measurement_text_tag;
        memcpy(meas.val.text, "\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4\xC3\xA4", 12);
        os = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&os, loraiot_Measurement_fields, &meas),
              "Measurement.text max (12 bytes) encodes");
        is = pb_istream_from_buffer(buf, os.bytes_written);
        loraiot_Measurement measIn = loraiot_Measurement_init_zero;
        CHECK(pb_decode(&is, loraiot_Measurement_fields, &measIn) &&
              measIn.which_val == loraiot_Measurement_text_tag &&
              memcmp(measIn.val.text, meas.val.text, 12) == 0,
              "Measurement.text max (12 bytes) round trip");
    }

    /* Max+1: wire payload with 13 bytes of text must be rejected */
    {
        /* metric_id=2 (0x08 0x02), field 3 text tag 0x1A, length 13 = 0x0D, then 13 'x' */
        static const uint8_t over13[] = {
            0x08, 0x02, 0x1A, 0x0D,
            'x','x','x','x','x','x','x','x','x','x','x','x','x'
        };
        loraiot_Measurement measOver = loraiot_Measurement_init_zero;
        is = pb_istream_from_buffer(over13, sizeof(over13));
        CHECK(!pb_decode(&is, loraiot_Measurement_fields, &measOver),
              "Measurement.text max+1 (13 bytes on wire) rejected at decode");
    }

    /* ------------------------------------------------------------------ */
    printf("\n=== Data.values (max 16 entries) ===\n");
    /* ------------------------------------------------------------------ */

    /* Max: Data with exactly 16 measurements encodes and round-trips */
    {
        loraiot_Data d = loraiot_Data_init_zero;
        d.device = 1;
        d.values_count = DATA_VALUES_MAX;
        for (uint32_t i = 0; i < DATA_VALUES_MAX; i++) {
            d.values[i].metric_id = i + 1;
            d.values[i].which_val = loraiot_Measurement_num_tag;
            d.values[i].val.num   = (int32_t)i;
        }
        os = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&os, loraiot_Data_fields, &d),
              "Data.values max (16 entries) encodes");
        printf("     Data 16 numeric values size: %u B\n", (unsigned)os.bytes_written);
        is = pb_istream_from_buffer(buf, os.bytes_written);
        loraiot_Data dIn = loraiot_Data_init_zero;
        CHECK(pb_decode(&is, loraiot_Data_fields, &dIn) && dIn.values_count == DATA_VALUES_MAX,
              "Data.values max (16 entries) round trip");
    }

    /* Max+1: hand-craft a Data wire payload with 17 measurements and verify rejection.
     * Each Measurement entry: tag 0x12 (field 2, LEN), length 2, metric_id field (0x08 0xNN).
     * We produce device field then 17 repeated field-2 entries. */
    {
        static uint8_t over17[2 + 17 * 4];
        size_t pos = 0;
        over17[pos++] = 0x08; over17[pos++] = 0x01;  /* device = 1 */
        for (uint8_t i = 0; i < 17; i++) {
            over17[pos++] = 0x12;       /* field 2 (values), wire type 2 */
            over17[pos++] = 0x02;       /* length = 2 */
            over17[pos++] = 0x08;       /* field 1 (metric_id), wire type 0 */
            over17[pos++] = (uint8_t)(i + 1);
        }
        loraiot_Data dOver = loraiot_Data_init_zero;
        is = pb_istream_from_buffer(over17, pos);
        CHECK(!pb_decode(&is, loraiot_Data_fields, &dOver),
              "Data.values max+1 (17 entries on wire) rejected at decode");
    }

    /* ------------------------------------------------------------------ */
    printf("\n=== Echo.text (max 175 chars, buf 176) ===\n");
    /* ------------------------------------------------------------------ */

    /* Max: exactly 175-character Echo encodes and round-trips */
    {
        loraiot_Echo e = loraiot_Echo_init_zero;
        memset(e.text, 'e', ECHO_TEXT_MAX);  /* 175 'e', NUL at [175] */
        os = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&os, loraiot_Echo_fields, &e),
              "Echo.text max (175 chars) encodes");
        printf("     Echo max size: %u B\n", (unsigned)os.bytes_written);
        is = pb_istream_from_buffer(buf, os.bytes_written);
        loraiot_Echo eIn = loraiot_Echo_init_zero;
        CHECK(pb_decode(&is, loraiot_Echo_fields, &eIn) && strlen(eIn.text) == ECHO_TEXT_MAX,
              "Echo.text max (175 chars) round trip");
    }

    /* Max+1: wire payload with a 176-byte Echo text must be rejected */
    {
        /* field 1 (text), wire type 2: tag = 0x0A, length 176 (two-byte varint: 0xB0 0x01) */
        static uint8_t over176[3 + 176];
        over176[0] = 0x0A;   /* tag: field 1, wire type 2 */
        over176[1] = 0xB0;   /* length 176 low 7 bits: 176 & 0x7F = 48, with continuation: 0x80 | 48 = 0xB0 */
        over176[2] = 0x01;   /* length 176 high bits: 176 >> 7 = 1 */
        memset(over176 + 3, 'e', 176);
        loraiot_Echo eOver = loraiot_Echo_init_zero;
        is = pb_istream_from_buffer(over176, sizeof(over176));
        CHECK(!pb_decode(&is, loraiot_Echo_fields, &eOver),
              "Echo.text max+1 (176 chars on wire) rejected at decode");
    }

    /* ------------------------------------------------------------------ */
    printf("\n=== Full message round trips ===\n");
    /* ------------------------------------------------------------------ */

    /* DATA: Sensor A - Voltage 3.35 V, Current -0.12 A, Date "30.10.2026" (text) */
    {
        loraiot_Data data = loraiot_Data_init_zero;
        data.device = 1;
        data.values_count = 3;
        data.values[0].metric_id = 1;
        data.values[0].which_val = loraiot_Measurement_num_tag;
        data.values[0].val.num = 335;
        data.values[1].metric_id = 2;
        data.values[1].which_val = loraiot_Measurement_num_tag;
        data.values[1].val.num = -120;
        data.values[2].metric_id = 3;
        data.values[2].which_val = loraiot_Measurement_text_tag;
        strcpy(data.values[2].val.text, "30.10.2026");
        os = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&os, loraiot_Data_fields, &data),
              "encode Data (Voltage + Current + Date text)");
        hex("Data payload", buf, os.bytes_written);
        is = pb_istream_from_buffer(buf, os.bytes_written);
        loraiot_Data dataIn = loraiot_Data_init_zero;
        CHECK(pb_decode(&is, loraiot_Data_fields, &dataIn) &&
              dataIn.values_count == 3 &&
              dataIn.values[0].val.num == 335 &&
              dataIn.values[1].val.num == -120 &&
              strcmp(dataIn.values[2].val.text, "30.10.2026") == 0,
              "Data round trip incl. negative sint32 and text");
    }

    /* RESPONSE(Device): Sensor A with 4 metrics */
    {
        loraiot_Response rsp = loraiot_Response_init_zero;
        rsp.request_seq = 7;
        rsp.which_result = loraiot_Response_def_tag;
        loraiot_Device *dev = &rsp.result.def;
        dev->device = 1;
        strcpy(dev->name, "Sensor A");
        static const char *names[] = {"Voltage", "Current", "Temperature", "Humidity"};
        static const char *units[] = {"V", "A", "degC", "%RH"};
        const size_t metricCount = sizeof(names) / sizeof(names[0]);
        dev->metrics_count = (pb_size_t)metricCount;
        for (size_t i = 0; i < metricCount; i++) {
            dev->metrics[i].id = (uint32_t)(i + 1);
            strcpy(dev->metrics[i].name, names[i]);
            strcpy(dev->metrics[i].unit, units[i]);
            dev->metrics[i].scale = TEST_METRIC_SCALE;
        }
        os = pb_ostream_from_buffer(buf, sizeof(buf));
        CHECK(pb_encode(&os, loraiot_Response_fields, &rsp), "encode Response(Device)");
        printf("     Response(Device) size: %u B\n", (unsigned)os.bytes_written);
        is = pb_istream_from_buffer(buf, os.bytes_written);
        loraiot_Response rspIn = loraiot_Response_init_zero;
        CHECK(pb_decode(&is, loraiot_Response_fields, &rspIn) &&
              rspIn.which_result == loraiot_Response_def_tag &&
              strcmp(rspIn.result.def.name, "Sensor A") == 0 &&
              rspIn.result.def.metrics_count == metricCount &&
              strcmp(rspIn.result.def.metrics[2].name, "Temperature") == 0,
              "Response(Device) round trip");
    }

    /* ------------------------------------------------------------------ */
    printf("\n=== Generated worst-case size macros ===\n");
    /* ------------------------------------------------------------------ */
    printf("     loraiot_Measurement_size = %d\n", (int)loraiot_Measurement_size);
    printf("     loraiot_Data_size        = %d\n", (int)loraiot_Data_size);
    printf("     loraiot_Device_size      = %d\n", (int)loraiot_Device_size);
    printf("     loraiot_Response_size    = %d\n", (int)loraiot_Response_size);

    printf("\n%s\n", failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED");
    return failures ? 1 : 0;
}
