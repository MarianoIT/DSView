#include <libsigrokdecode.h>
#include <libsigrok.h>
#include "libsigrok-internal.h"
#undef min
#undef max
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "sigrok/decode-input.h"

struct Annotation { uint64_t start, end; int kind, type; std::string hex, text; unsigned texts; };
static void annotation(srd_proto_data *packet, void *user)
{
    auto *data = static_cast<srd_proto_data_annotation *>(packet->data);
    auto &output = *static_cast<std::vector<Annotation> *>(user);
    output.push_back({packet->start_sample, packet->end_sample, data->ann_class,
        data->ann_type, data->str_number_hex, data->ann_text && data->ann_text[0] ? data->ann_text[0] : "", data->ann_text ? g_strv_length(data->ann_text) : 0});
}
static bool decode(const char *id, const std::map<std::string, int> &channels,
    const std::vector<uint8_t> &samples, unsigned width, std::vector<Annotation> &output,
    bool expect_error = false, const char *stack = nullptr)
{
    srd_session *session = nullptr;
    if (srd_session_new(&session) != SRD_OK) return false;
    GHashTable *options = g_hash_table_new(g_str_hash, g_str_equal);
    auto *instance = srd_inst_new(session, id, options);
    auto *child = stack ? srd_inst_new(session, stack, options) : nullptr;
    g_hash_table_destroy(options);
    bool ok = instance != nullptr;
    if (stack) ok = ok && child && srd_inst_stack(session, instance, child) == SRD_OK;
    GHashTable *mapping = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
    for (const auto &entry : channels)
        g_hash_table_insert(mapping, g_strdup(entry.first.c_str()), g_variant_ref_sink(g_variant_new_int32(entry.second)));
    if (ok) ok = srd_inst_channel_set_all(instance, mapping) == SRD_OK;
    g_hash_table_destroy(mapping);
    if (ok) ok = srd_session_metadata_set(session, SRD_CONF_SAMPLERATE, g_variant_new_uint64(1000000)) == SRD_OK;
    if (ok) ok = srd_pd_output_callback_add(session, SRD_OUTPUT_ANN, annotation, &output) == SRD_OK;
    if (ok) ok = srd_session_start(session) == SRD_OK;
    // This must fail before the decoder reads past a one-byte buffer.
    if (ok) ok = srd_session_send(session, 0, 100, samples.data(), 1, width) == SRD_ERR_ARG;
    int result = SRD_OK;
    for (size_t start = 0; ok && start < samples.size() / width;) {
        size_t end = std::min(samples.size() / width, start + 17); // Edges cross chunk boundaries.
        result = srd_session_send(session, start, end, samples.data() + start * width, (end - start) * width, width);
        if (result != SRD_OK) break;
        start = end;
    }
    if (expect_error) ok = ok && result != SRD_OK;
    else {
        char *error = nullptr;
        ok = ok && result == SRD_OK && ds_srd_session_end(session, &error) == SRD_OK;
        g_free(error);
    }
    srd_session_destroy(session);
    return ok;
}

static std::vector<uint8_t> raw;
static std::atomic<bool> complete{false}, capture_error{false};
static void capture_data(const sr_dev_inst *, const sr_datafeed_packet *packet)
{
    if (packet->status != SR_PKT_OK) capture_error = true;
    if (packet->type == SR_DF_END) complete = true;
    if (packet->type != SR_DF_DSO || packet->status != SR_PKT_OK) return;
    auto *data = static_cast<const sr_datafeed_dso *>(packet->payload);
    size_t enabled = 0;
    for (GSList *entry = data->probes; entry; entry = entry->next)
        if (static_cast<sr_channel *>(entry->data)->enabled) enabled++;
    auto *bytes = static_cast<const uint8_t *>(data->data);
    for (int i = 0; i < data->num_samples; i++) raw.push_back(bytes[i * enabled]);
}
static void capture_event(int event)
{
    if (event == DS_EV_COLLECT_TASK_END_BY_ERROR || event == DS_EV_COLLECT_TASK_END_BY_DETACHED) {
        capture_error = true; complete = true;
    }
}
static bool hardware()
{
    ds_set_firmware_resource_dir(DSVIEW_CLI_RESOURCE_DIR);
    ds_set_datafeed_callback(capture_data);
    ds_set_event_callback(capture_event);
    if (ds_lib_init() != SR_OK) return false;
    ds_device_base_info *devices = nullptr;
    int count = 0;
    bool ok = ds_get_device_list(&devices, &count) == SR_OK && count > 0 && ds_active_device(devices[0].handle) == SR_OK;
    ds_device_full_info info{};
    ok = ok && ds_get_actived_device_info(&info) == SR_OK && info.dev_type == DEV_TYPE_USB;
    auto set = [](int key, GVariant *value) { return ds_set_actived_device_config(nullptr, nullptr, key, value) == SR_OK; };
    ok = ok && set(SR_CONF_DEVICE_MODE, g_variant_new_int16(DSO)) &&
        set(SR_CONF_INSTANT, g_variant_new_boolean(TRUE)) &&
        set(SR_CONF_SAMPLERATE, g_variant_new_uint64(1000000)) &&
        set(SR_CONF_LIMIT_SAMPLES, g_variant_new_uint64(10000)) &&
        set(SR_CONF_TRIGGER_SOURCE, g_variant_new_byte(DSO_TRIGGER_AUTO));
    if (ok) ok = ds_start_collect() == SR_OK;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (ok && !complete && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ds_stop_collect();
    ok = ok && complete && !capture_error && raw.size() == 10000;
    if (ok) std::printf("Physical device: %s; samples=%zu\n", info.name, raw.size());
    g_free(devices);
    ds_lib_exit();
    if (!ok) return false;
    auto bounds = std::minmax_element(raw.begin(), raw.end());
    if (*bounds.first == *bounds.second) return false;
    unsigned threshold = *bounds.first + (*bounds.second - *bounds.first) / 2;
    std::vector<uint8_t> digital;
    for (auto value : raw) digital.push_back(value >= threshold);
    unsigned edges = 0;
    for (size_t i = 1; i < digital.size(); i++) if (!digital[i-1] && digital[i]) edges++;
    std::vector<Annotation> output;
    ok = edges > 2 && decode("clock", {{"clock", 0}}, digital, 1, output);
    unsigned frequencies = 0;
    for (auto &item : output) if (item.kind == 0) { frequencies++; std::puts(item.text.c_str()); }
    return ok && frequencies == edges - 1;
}
int main(int argc, char **argv)
{
    g_setenv("SIGROKDECODE_DIR", DSVIEW_DECODER_FIXTURES, TRUE);
    if (srd_init(DSVIEW_DECODER_DIR) != SRD_OK) return 1;
    bool ok = std::strcmp(srd_package_version_string_get(), "0.5.3") == 0;
    unsigned loaded = 0;
    GDir *directory = g_dir_open(DSVIEW_DECODER_DIR, 0, nullptr);
    const char *name;
    while (directory && (name = g_dir_read_name(directory))) {
        gchar *path = g_build_filename(DSVIEW_DECODER_DIR, name, "pd.py", nullptr);
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            int result = srd_decoder_load(name);
            if (result != SRD_OK) { std::fprintf(stderr, "Cannot load %s: %d\n", name, result); ok = false; }
            else loaded++;
        }
        g_free(path);
    }
    if (directory) g_dir_close(directory);
    ok = ok && loaded > 100;
    std::printf("libsigrokdecode=%s decoders=%u\n", srd_package_version_string_get(), loaded);
    if (argc == 2 && std::strcmp(argv[1], "--hardware") == 0) ok = ok && hardware();
    else {
        const uint8_t bits[] = {0xa0, 0x01};
        const uint8_t *planes[] = {bits, nullptr, nullptr};
        const uint8_t constants[] = {0, 1, 1};
        const unsigned offsets[] = {5, 0, 0};
        const int mapping[] = {9, 2, -1};
        const std::vector<uint8_t> expected = {4, 2, 4, 0, 4, 2, 4, 2};
        ok = dsview_interleave(planes, constants, offsets, mapping, 3, 4, 2) == expected && ok;
        for (const char *fixture : {"dsview_regression", "dsview_sink", "dsview_error"})
            ok = srd_decoder_load(fixture) == SRD_OK && ok;
        for (unsigned iteration = 0; ok && iteration < 20; iteration++) {
            std::vector<Annotation> flushed;
            ok = decode("dsview_regression", {{"data", 0}}, std::vector<uint8_t>(100), 1,
                flushed, false, "dsview_sink") && flushed.size() == 2;
            if (ok) ok = flushed[0].end == 100 && flushed[0].hex == "A5" &&
                flushed[0].texts == 12 && flushed[0].type == 108 &&
                flushed[1].end == 100 && flushed[1].hex == "FFFFFFFFFFFFFFFF";
        }
        std::vector<Annotation> failed;
        ok = decode("dsview_error", {{"data", 0}}, std::vector<uint8_t>(100), 1, failed, true) && ok;
        std::printf("Stacked completion, 12 texts, signed numbers and Python errors: %s\n", ok ? "passed" : "FAILED");
        for (unsigned iteration = 0; ok && iteration < 20; iteration++) {
            std::vector<uint8_t> clock(200);
            for (size_t i = 0; i < clock.size(); i++) clock[i] = (i % 10) >= 5;
            std::vector<Annotation> output;
            ok = decode("clock", {{"clock", 0}}, clock, 1, output) && output.size() == 38;
            for (const auto &item : output) ok = ok && item.end - item.start == 10;
        }
        std::vector<uint8_t> spi;
        auto append = [&](unsigned value, unsigned length) { for (unsigned i=0; i<length; i++) { spi.push_back(value & 255); spi.push_back(value >> 8); } };
        append(4, 8); append(0, 4);
        for (int bit = 7; bit >= 0; bit--) { unsigned data = ((0xa5 >> bit) & 1) << 9; append(data, 4); append(data | 1, 4); }
        append(0, 4); append(4, 8);
        std::vector<Annotation> output;
        ok = ok && decode("0:spi", {{"clk",0},{"mosi",9},{"cs",2}}, spi, 2, output);
        bool byte = false;
        for (auto &item : output) if (item.hex == "A5") byte = true;
        ok = ok && byte;
        std::printf("Clock chunks/repeated sessions and remapped SPI byte A5: %s\n", ok ? "passed" : "FAILED");
    }
    srd_exit();
    return ok ? 0 : 1;
}
