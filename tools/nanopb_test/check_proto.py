"""Checks loraiot.proto against the wire-size budget of doc/LoRaProtocol.md.

How it works, in short:
  1. protoc (the Protocol Buffers compiler) turns loraiot.proto into a Python
     module called loraiot_pb2. It contains one Python class per `message` in
     the .proto file (Beacon, Device, Data, Response, ...).
  2. We create objects of those classes and fill them with WORST-CASE values
     (the largest numbers and the longest strings the schema allows).
  3. message.SerializeToString() returns the exact bytes the message would
     occupy inside a LoRa frame, so len() of it is the "wire size".
  4. That size must not exceed PAYLOAD_LIMIT (the maximum frame payload).

Protobuf encoding is standardized, so the sizes measured here on the PC are the
same ones the firmware's nanopb code will produce. A schema change that breaks the
size budget is therefore caught before flashing.

Run from the repository root (see doc/NanopbGeneration.md):
    python tools/nanopb_test/check_proto.py

Needs: pip install grpcio-tools protobuf
Exit code 0 = all checks passed, 1 = at least one failed.
"""
import os
import sys
import tempfile

from grpc_tools import protoc

# The limits below are the maximums stated in the comments of loraiot.proto
# (strings and repeated fields have no size limit in protobuf itself). The tests
# encode messages at exactly these maximums to find the worst-case wire size.
PAYLOAD_LIMIT = 186          # max frame payload, doc/LoRaProtocol.md section 4
NAME_MAX = 12                # Metric.name and Device.name, characters
UNIT_MAX = 5                 # Metric.unit, characters
METRICS_MAX = 8              # Device.metrics
VALUES_MAX = 16              # Data.values
TEXT_MAX = 12                # Measurement.text, bytes (UTF-8)
ECHO_MAX = 175               # Echo.text, characters
# Protobuf integers are "varints": small values take 1 byte, large ones up to 5
# (or 10 for negative plain int32). Using the extremes gives the largest encoding.
# Measurement.value is a sint32, so INT32_MIN is its biggest encoding (5 bytes).
INT32_MIN = -2147483648
UINT32_MAX = 4294967295
PING_TEST_BOOT_ID = 0x12345678
PING_EXPECTED_HEX = "08F8ACD19101"   # worked example, doc/LoRaProtocol.md section 4.2
TEST_DEVICE_ID = 255         # any valid device id; 255 exercises the widest varint
REQUEST_SEQ_MAX = 65535      # largest SEQ (2 bytes in the frame header) encodes largest
TEST_SCALE = 1000

PROTO_DIR = os.path.join("src", "lora", "proto")


def load_loraiot_module():
    """Compile loraiot.proto with protoc and return the generated Python module.

    grpc_tools.protoc is the protoc compiler shipped as a Python package, so no
    separate protoc installation is needed. protoc.main() takes the same
    arguments as the command line tool and returns its exit code:
        -I<dir>          folder where .proto files (and their imports) are searched
        --python_out=<d> folder where the generated loraiot_pb2.py is written
    The first list element ("protoc") is only the program name, as in argv[0].
    """
    output_dir = tempfile.mkdtemp()   # throw-away folder for the generated file
    exit_code = protoc.main(["protoc", "-I" + PROTO_DIR, "--python_out=" + output_dir,
                             os.path.join(PROTO_DIR, "loraiot.proto")])
    if exit_code != 0:
        sys.exit("protoc failed")
    sys.path.insert(0, output_dir)    # make the generated file importable
    import loraiot_pb2
    return loraiot_pb2


def build_loraiot_device_message(loraiot_module, metric_count, name_len, unit_len):
    """Build a Device message with `metric_count` metrics and fixed-length strings.

    `loraiot_module` is the generated module. loraiot_module.Device(...) creates
    a message, with fields given as keyword arguments. `metrics` is a `repeated`
    field (a list of sub-messages): .add() appends a new empty Metric and returns
    it so that its fields can be filled in.
    """
    loraiot_device_message = loraiot_module.Device(device=TEST_DEVICE_ID, name="N" * name_len)
    for metric_id in range(1, metric_count + 1):
        loraiot_metric_message = loraiot_device_message.metrics.add()
        loraiot_metric_message.id = metric_id
        loraiot_metric_message.name = "n" * name_len
        loraiot_metric_message.unit = "u" * unit_len
        loraiot_metric_message.scale = TEST_SCALE
    return loraiot_device_message


def main():
    # The generated classes live here: loraiot_module.Beacon, .Device, .Data, .Response, ...
    loraiot_module = load_loraiot_module()
    failures = 0

    def check_wire_size(label, size, must_fit=True, expect=None):
        """Print one OK/FAIL line for an encoded message of `size` bytes.

        must_fit=True  : passes if size <= PAYLOAD_LIMIT.
        must_fit=False : passes if size >  PAYLOAD_LIMIT (documents a known overflow).
        expect         : if given, the size must also equal this exact byte count.
        """
        nonlocal failures      # lets this inner function modify main()'s counter
        fits = size <= PAYLOAD_LIMIT
        passed = (fits == must_fit) and (expect is None or size == expect)
        failures += 0 if passed else 1
        print("%-4s %-52s %4d B  (limit %d)" % ("OK" if passed else "FAIL", label, size, PAYLOAD_LIMIT))

    # Worked example of doc/LoRaProtocol.md section 4.2.
    # SerializeToString() returns Python `bytes` (despite the name): the encoded message.
    # A PING is a Beacon with only boot_id set, so the other fields are not sent.
    loraiot_ping_message = loraiot_module.Beacon(boot_id=PING_TEST_BOOT_ID)
    ping_payload = loraiot_ping_message.SerializeToString()
    passed = ping_payload.hex().upper() == PING_EXPECTED_HEX
    failures += 0 if passed else 1
    print("%-4s %-52s %s" % ("OK" if passed else "FAIL", "PING payload equals worked example",
                             ping_payload.hex(" ").upper()))

    # An absent optional field must stay absent on the wire (PING vs BEACON).
    # For an `optional` field, "set to 0" and "never set" are different things:
    # 0 is still transmitted, an unset field costs no bytes. HasField() tells them apart.
    # (A non-optional field set to 0 would be dropped, as 0 is the default.)
    loraiot_beacon_message_with_zeros = loraiot_module.Beacon(
        boot_id=1, fw_version=0, uptime_s=0, registry_rev=0)
    loraiot_beacon_message_without_optionals = loraiot_module.Beacon(boot_id=1)
    size_with_zeros = len(loraiot_beacon_message_with_zeros.SerializeToString())
    size_without_optionals = len(loraiot_beacon_message_without_optionals.SerializeToString())
    passed = loraiot_beacon_message_with_zeros.HasField("registry_rev") \
        and not loraiot_beacon_message_without_optionals.HasField("registry_rev") \
        and size_with_zeros > size_without_optionals
    failures += 0 if passed else 1
    print("%-4s %-52s" % ("OK" if passed else "FAIL", "optional registry_rev=0 is sent, absent is not"))

    # Worst-case Data message: largest age_ms, alert set, and the maximum number of
    # values, each with the most expensive sint32 value (5 B) vs 12 B text value (14 B with tag+len).
    # Text (12 B) is larger than sint32 (5 B).
    loraiot_data_message = loraiot_module.Data(device=TEST_DEVICE_ID, age_ms=UINT32_MAX, alert=True)
    for metric_id in range(1, VALUES_MAX + 1):
        loraiot_measurement_message = loraiot_data_message.values.add()
        loraiot_measurement_message.metric_id = metric_id
        loraiot_measurement_message.num = INT32_MIN
    check_wire_size("Data, 16 worst-case numeric values", len(loraiot_data_message.SerializeToString()))

    loraiot_data_text_message = loraiot_module.Data(device=TEST_DEVICE_ID, age_ms=UINT32_MAX, alert=True)
    for metric_id in range(1, VALUES_MAX + 1):
        loraiot_measurement_message = loraiot_data_text_message.values.add()
        loraiot_measurement_message.metric_id = metric_id
        loraiot_measurement_message.text = "x" * TEXT_MAX
    check_wire_size("Data, 16 worst-case text values (must NOT fit)", len(loraiot_data_text_message.SerializeToString()), must_fit=False)

    loraiot_typical_device_message = build_loraiot_device_message(loraiot_module, 4, 7, 4)
    check_wire_size("Device, 4 typical metrics",
                    len(loraiot_typical_device_message.SerializeToString()))
    # Known limitation: the schema bound (8) allows a Device that does not fit one frame.
    # The REMOTE must reject such a registration (doc/LoRaProtocol.md section 10).
    loraiot_oversized_device_message = build_loraiot_device_message(
        loraiot_module, METRICS_MAX, NAME_MAX, UNIT_MAX)
    check_wire_size("Device, 8 metrics at maximum length (must NOT fit)",
                    len(loraiot_oversized_device_message.SerializeToString()), must_fit=False)

    # The Response wrapper around a Device adds request_seq, status and the oneof tag.
    # Response has a `oneof result`: at most one of its fields (data, def, echo) is
    # set at a time. Setting one automatically clears the others.
    # A sub-message field cannot be assigned with "=", so CopyFrom() copies one into it.
    # The Device field is named `def` in the .proto, but `def` is a reserved word in
    # Python, so loraiot_response_message.def is a syntax error;
    # getattr(loraiot_response_message, "def") reaches it by string.
    # REQUEST_SEQ_MAX is the largest request_seq, so it encodes largest.
    # status is also set to a non-default value, because a default (0) would not be sent.
    loraiot_response_message = loraiot_module.Response(
        request_seq=REQUEST_SEQ_MAX, status=loraiot_module.STATUS_BAD_REQUEST)
    getattr(loraiot_response_message, "def").CopyFrom(
        build_loraiot_device_message(loraiot_module, 4, NAME_MAX, UNIT_MAX))
    check_wire_size("Response(Device), 4 metrics at maximum length",
                    len(loraiot_response_message.SerializeToString()))

    # Informational: find the largest Device that still fits in a Response.
    largest_fitting_count = 0
    for metric_count in range(1, METRICS_MAX + 1):
        loraiot_response_message = loraiot_module.Response(
            request_seq=REQUEST_SEQ_MAX, status=loraiot_module.STATUS_BAD_REQUEST)
        getattr(loraiot_response_message, "def").CopyFrom(
            build_loraiot_device_message(loraiot_module, metric_count, NAME_MAX, UNIT_MAX))
        if len(loraiot_response_message.SerializeToString()) <= PAYLOAD_LIMIT:
            largest_fitting_count = metric_count
    print("INFO largest Device that fits when every metric is at maximum length: %d metrics"
          % largest_fitting_count)

    loraiot_response_message = loraiot_module.Response(
        request_seq=REQUEST_SEQ_MAX, status=loraiot_module.STATUS_BAD_REQUEST)
    loraiot_response_message.data.CopyFrom(loraiot_data_message)
    check_wire_size("Response(Data), 16 worst-case values",
                    len(loraiot_response_message.SerializeToString()))

    # Assigning to a field of a sub-message (loraiot_response_message.echo.text)
    # creates and selects that oneof member automatically, so no CopyFrom() is needed.
    loraiot_response_message = loraiot_module.Response(request_seq=REQUEST_SEQ_MAX)
    loraiot_response_message.echo.text = "x" * ECHO_MAX
    check_wire_size("Response(Echo), 175 characters",
                    len(loraiot_response_message.SerializeToString()))

    print("\n%s" % ("ALL CHECKS PASSED" if failures == 0 else "%d CHECK(S) FAILED" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
