"""Checks loraiot.proto against the wire-size budget of doc/LoRaProtocol.md.

Run from the repository root (see doc/NanopbGeneration.md):
    python tools/nanopb_test/check_proto.py

Needs: pip install grpcio-tools protobuf
Exit code 0 = all checks passed, 1 = at least one failed.
"""
import os
import sys
import tempfile

from grpc_tools import protoc

PAYLOAD_LIMIT = 186          # max frame payload, doc/LoRaProtocol.md section 4
NAME_MAX = 12                # Metric.name and Device.name, characters
UNIT_MAX = 5                 # Metric.unit, characters
METRICS_MAX = 8              # Device.metrics
VALUES_MAX = 16              # Data.values
ECHO_MAX = 175               # Echo.text, characters
INT32_MIN = -2147483648
UINT32_MAX = 4294967295
PING_TEST_BOOT_ID = 0x12345678
PING_EXPECTED_HEX = "08F8ACD19101"   # worked example, doc/LoRaProtocol.md section 4.2
TEST_DEVICE_ID = 255         # any valid device id; 255 exercises the widest varint
TEST_SCALE = 1000

PROTO_DIR = os.path.join("src", "lora", "proto")


def load_module():
    out = tempfile.mkdtemp()
    rc = protoc.main(["protoc", "-I" + PROTO_DIR, "--python_out=" + out,
                      os.path.join(PROTO_DIR, "loraiot.proto")])
    if rc != 0:
        sys.exit("protoc failed")
    sys.path.insert(0, out)
    import loraiot_pb2
    return loraiot_pb2


def device(p, count, name_len, unit_len):
    d = p.Device(device=TEST_DEVICE_ID, name="N" * name_len)
    for i in range(1, count + 1):
        m = d.metrics.add()
        m.id = i
        m.name = "n" * name_len
        m.unit = "u" * unit_len
        m.scale = TEST_SCALE
    return d


def main():
    p = load_module()
    failures = 0

    def check(label, size, must_fit=True, expect=None):
        nonlocal failures
        fits = size <= PAYLOAD_LIMIT
        ok = (fits == must_fit) and (expect is None or size == expect)
        failures += 0 if ok else 1
        print("%-4s %-52s %4d B  (limit %d)" % ("OK" if ok else "FAIL", label, size, PAYLOAD_LIMIT))

    # Worked example of doc/LoRaProtocol.md section 4.2
    ping = p.Beacon(boot_id=PING_TEST_BOOT_ID).SerializeToString()
    ok = ping.hex().upper() == PING_EXPECTED_HEX
    failures += 0 if ok else 1
    print("%-4s %-52s %s" % ("OK" if ok else "FAIL", "PING payload equals worked example", ping.hex(" ").upper()))

    # An absent optional field must stay absent on the wire (PING vs BEACON)
    beacon = p.Beacon(boot_id=1, fw_version=0, uptime_s=0, registry_rev=0)
    absent = p.Beacon(boot_id=1)
    ok = beacon.HasField("registry_rev") and not absent.HasField("registry_rev") \
        and len(beacon.SerializeToString()) > len(absent.SerializeToString())
    failures += 0 if ok else 1
    print("%-4s %-52s" % ("OK" if ok else "FAIL", "optional registry_rev=0 is sent, absent is not"))

    d = p.Data(device=TEST_DEVICE_ID, age_ms=UINT32_MAX, alert=True)
    for i in range(1, VALUES_MAX + 1):
        m = d.values.add()
        m.metric_id = i
        m.value = INT32_MIN
    check("Data, 16 worst-case values", len(d.SerializeToString()))

    check("Device, 4 typical metrics",
          len(device(p, 4, 7, 4).SerializeToString()))
    # Known limitation: the schema bound (8) allows a Device that does not fit one frame.
    # The REMOTE must reject such a registration (doc/LoRaProtocol.md section 10).
    check("Device, 8 metrics at maximum length (must NOT fit)",
          len(device(p, METRICS_MAX, NAME_MAX, UNIT_MAX).SerializeToString()), must_fit=False)

    # The Response wrapper around a Device adds request_seq, status and the oneof tag.
    r = p.Response(request_seq=65535, status=p.STATUS_BAD_REQUEST)
    getattr(r, "def").CopyFrom(device(p, 4, NAME_MAX, UNIT_MAX))   # "def" is a Python keyword
    check("Response(Device), 4 metrics at maximum length", len(r.SerializeToString()))

    fitting = 0
    for n in range(1, METRICS_MAX + 1):
        r = p.Response(request_seq=65535, status=p.STATUS_BAD_REQUEST)
        getattr(r, "def").CopyFrom(device(p, n, NAME_MAX, UNIT_MAX))
        if len(r.SerializeToString()) <= PAYLOAD_LIMIT:
            fitting = n
    print("INFO largest Device that fits when every metric is at maximum length: %d metrics" % fitting)

    r = p.Response(request_seq=65535, status=p.STATUS_BAD_REQUEST)
    r.data.CopyFrom(d)
    check("Response(Data), 16 worst-case values", len(r.SerializeToString()))

    r = p.Response(request_seq=65535)
    r.echo.text = "x" * ECHO_MAX
    check("Response(Echo), 175 characters", len(r.SerializeToString()))

    print("\n%s" % ("ALL CHECKS PASSED" if failures == 0 else "%d CHECK(S) FAILED" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
