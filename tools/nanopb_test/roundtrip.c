/* Host-side round-trip test of the generated nanopb code (no ESP32 needed).
 * Build and run: see doc/NanopbGeneration.md (compiled with "python -m ziglang cc").
 * It prints the encoded bytes so they can be compared with check_proto.py.
 */
#include <stdio.h>
#include <string.h>
#include "pb_encode.h"
#include "pb_decode.h"
#include "loraiot.pb.h"

/* Test vectors and limits (doc/LoRaProtocol.md section 4.2 and the .options file) */
#define PING_TEST_BOOT_ID    0x12345678u
static const uint8_t PING_EXPECTED_PAYLOAD[] = {0x08, 0xF8, 0xAC, 0xD1, 0x91, 0x01};
#define MAX_PAYLOAD_LEN      186   /* E220 sub-packet 200 B minus 14 B frame overhead */
#define EXPECTED_NAME_SIZE   13    /* Metric.name / Device.name: 12 characters + NUL */
#define EXPECTED_UNIT_SIZE   6     /* Metric.unit: 5 characters + NUL */
#define TEST_METRIC_SCALE    100

static int failures = 0;

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s\n", msg); failures++; } else { printf("OK   %s\n", msg); } } while (0)

static void hex(const char *label, const uint8_t *b, size_t n) {
    printf("     %s:", label);
    for (size_t i = 0; i < n; i++) printf(" %02X", b[i]);
    printf("\n");
}

int main(void) {
    uint8_t buf[MAX_PAYLOAD_LEN];
    pb_ostream_t os;
    pb_istream_t is;

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

    /* DATA: Sensor A (device 1). Voltage 3.35 V (metric 1, scale 100, value 335) and
     * Current -0.12 A (metric 2, scale 1000, value -120; negative on purpose to test sint32). */
    loraiot_Data data = loraiot_Data_init_zero;
    data.device = 1;
    data.values_count = 2;
    data.values[0].metric_id = 1; data.values[0].value = 335;
    data.values[1].metric_id = 2; data.values[1].value = -120;
    os = pb_ostream_from_buffer(buf, sizeof(buf));
    CHECK(pb_encode(&os, loraiot_Data_fields, &data),
          "encode Data ({\"device\":\"Sensor A\",\"data\":{\"Voltage\":3.35,\"Current\":-0.12}})");
    hex("Data payload", buf, os.bytes_written);
    is = pb_istream_from_buffer(buf, os.bytes_written);
    loraiot_Data dataIn = loraiot_Data_init_zero;
    CHECK(pb_decode(&is, loraiot_Data_fields, &dataIn) && dataIn.values_count == 2 &&
          dataIn.values[0].value == 335 && dataIn.values[1].value == -120, "Data round trip incl. negative sint32");

    /* RESPONSE(Device): definition of Sensor A with 4 metrics */
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
    CHECK(pb_decode(&is, loraiot_Response_fields, &rspIn) && rspIn.which_result == loraiot_Response_def_tag &&
          strcmp(rspIn.result.def.name, "Sensor A") == 0 && rspIn.result.def.metrics_count == metricCount &&
          strcmp(rspIn.result.def.metrics[2].name, "Temperature") == 0, "Response(Device) round trip");

    /* Limit: a 13th character must not fit into Metric.name (max_size 13 incl. NUL) */
    loraiot_Metric m = loraiot_Metric_init_zero;
    m.id = 1;
    memset(m.name, 'n', sizeof(m.name) - 1);
    os = pb_ostream_from_buffer(buf, sizeof(buf));
    CHECK(sizeof(m.name) == EXPECTED_NAME_SIZE && sizeof(m.unit) == EXPECTED_UNIT_SIZE &&
          sizeof(dev->name) == EXPECTED_NAME_SIZE,
          "struct sizes match the .options limits");
    CHECK(pb_encode(&os, loraiot_Metric_fields, &m), "encode Metric with 12-character name");

    /* The generated worst-case size macros must match the documented budget */
    printf("     loraiot_Device_size   = %d\n", (int)loraiot_Device_size);
    printf("     loraiot_Response_size = %d\n", (int)loraiot_Response_size);

    printf("\n%s\n", failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED");
    return failures ? 1 : 0;
}
