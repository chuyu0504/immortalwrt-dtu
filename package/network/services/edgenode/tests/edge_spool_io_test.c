#include "edge_config.h"
#include "edge_protocol.h"
#include "edge_spool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool edge_config_parse_uuid(const char *text, uint8_t output[16]) {
    unsigned value;
    for (size_t i = 0, pos = 0; i < 16; ++i) {
        if (text[pos] == '-') ++pos;
        if (sscanf(text + pos, "%2x", &value) != 1) return false;
        output[i] = (uint8_t)value; pos += 2;
    }
    return true;
}
void edge_config_format_uuid(const uint8_t value[16], char output[37]) {
    size_t pos = 0;
    for (size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) output[pos++] = '-';
        snprintf(output + pos, 3, "%02x", value[i]); pos += 2;
    }
}
static void require(bool ok, const char *reason) {
    if (!ok) { fprintf(stderr, "%s\n", reason); exit(1); }
}
static size_t record_wire(const uint8_t platform[16], const uint8_t id[16],
                          iot_edge_v1_Protocol protocol, uint64_t sequence, uint8_t *wire) {
    const uint8_t random[10] = {2};
    iot_edge_v1_Envelope envelope;
    require(edge_protocol_init_envelope(&envelope, platform, NULL, 1, sequence, sequence, random), "init envelope");
    envelope.which_payload = iot_edge_v1_Envelope_telemetry_batch_tag;
    envelope.payload.telemetry_batch.records_count = 1;
    iot_edge_v1_TelemetryRecord *record = &envelope.payload.telemetry_batch.records[0];
    record->record_id.size = 16; memcpy(record->record_id.bytes, id, 16);
    record->protocol = protocol;
    record->configuration_sha256.size = 32;
    memset(record->configuration_sha256.bytes, 0xa5, 32);
    size_t size = 0; const char *error = NULL;
    require(edge_protocol_encode(&envelope, wire, EDGENODE_MAX_WS_MESSAGE, &size, &error), "encode record");
    return size;
}
int main(void) {
    uint8_t platform[16] = {0}, one[16] = {1}, two[16] = {2}, regular[16] = {3}, other[16] = {4};
    unsigned pid = (unsigned)getpid(); memcpy(platform, &pid, sizeof(pid));
    uint8_t first[EDGENODE_MAX_WS_MESSAGE], second[EDGENODE_MAX_WS_MESSAGE], normal[EDGENODE_MAX_WS_MESSAGE], retry[EDGENODE_MAX_WS_MESSAGE];
    size_t a = record_wire(platform, one, iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO, 1, first);
    size_t b = record_wire(platform, two, iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO, 1, second);
    size_t c = record_wire(platform, regular, iot_edge_v1_Protocol_PROTOCOL_MODBUS, 1, normal);
    size_t retry_size = record_wire(platform, one, iot_edge_v1_Protocol_PROTOCOL_EXTERNAL_IO, 2, retry);
    require(a == b && b == c, "unexpected record sizes");
    edge_spool spool;
    require(edge_spool_init(&spool, platform, a * 2), "initialize spool");
    require(edge_spool_outbox_put_priority(&spool, one, first, a, true), "save first IO record");
    require(edge_spool_outbox_put(&spool, regular, normal, c), "save regular record");
    require(edge_spool_outbox_put_priority(&spool, two, second, b, true), "save second IO record");
    require(spool.outbox.count == 2 && !edge_spool_outbox_ack(&spool, regular), "priority did not evict only regular record");
    edge_spool_free(&spool);
    require(edge_spool_init(&spool, platform, 1), "restore reduced-limit spool");
    require(spool.outbox.count == 2, "protected backlog lost on restart");
    const edge_memory_message *saved = edge_spool_outbox_first(&spool);
    require(saved != NULL && saved->payload_size == a && memcmp(saved->payload, first, a) == 0,
            "restored sample changed its original configuration identity");
    require(edge_spool_outbox_put_priority(&spool, one, retry, retry_size, true), "same payload in a new envelope rejected after limit reduction");
    require(!edge_spool_outbox_put_priority(&spool, one, second, b, true), "conflicting duplicate accepted");
    require(!edge_spool_outbox_put_priority(&spool, other, first, a, true), "new message exceeded reduced limit");
    require(edge_spool_outbox_ack(&spool, one) && edge_spool_outbox_ack(&spool, two), "ack protected records");
    char directory[160], outbox[192]; snprintf(directory, sizeof(directory), "%s", spool.directory);
    edge_spool_free(&spool);
    snprintf(outbox, sizeof(outbox), "%s/outbox", directory);
    require(rmdir(outbox) == 0 && rmdir(directory) == 0, "clean test spool");
    return 0;
}
