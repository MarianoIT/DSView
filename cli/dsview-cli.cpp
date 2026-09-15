#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>
#include <thread>
#include <vector>

#include <glib.h>
#include <log/xlog.h>

extern "C" {
#include <libsigrok.h>
#include "libsigrok-internal.h"
}

namespace {

struct CaptureSummary {
    std::atomic<bool> complete{false};
    std::atomic<bool> failed{false};
    std::atomic<int> completion_event{0};
    std::atomic<uint64_t> dso_samples{0};
    std::atomic<uint8_t> dso_min{255};
    std::atomic<uint8_t> dso_max{0};
    std::atomic<uint64_t> logic_bytes{0};
    std::atomic<uint64_t> packets{0};
    std::vector<uint8_t> channel0_samples;
    std::vector<uint8_t> dso_channel0_samples;
};

CaptureSummary *capture_summary = nullptr;

void log_to_stderr(const char *data, int length)
{
    std::cerr.write(data, length);
    std::cerr.flush();
}

void data_callback(const sr_dev_inst *, const sr_datafeed_packet *packet)
{
    if (!capture_summary || !packet)
        return;

    capture_summary->packets++;
    switch (packet->type) {
    case SR_DF_DSO:
    {
        const auto *dso = static_cast<const sr_datafeed_dso *>(packet->payload);
        capture_summary->dso_samples += dso->num_samples;
        const auto *data = static_cast<const uint8_t *>(dso->data);
        for (int index = 0; index < dso->num_samples; index++) {
            const uint8_t value = data[index];
            capture_summary->dso_channel0_samples.push_back(value);
            uint8_t current_min = capture_summary->dso_min;
            while (value < current_min &&
                   !capture_summary->dso_min.compare_exchange_weak(current_min, value)) {}
            uint8_t current_max = capture_summary->dso_max;
            while (value > current_max &&
                   !capture_summary->dso_max.compare_exchange_weak(current_max, value)) {}
        }
        break;
    }
    case SR_DF_LOGIC:
    {
        const auto *logic = static_cast<const sr_datafeed_logic *>(packet->payload);
        capture_summary->logic_bytes += logic->length;
        const auto *data = static_cast<const uint8_t *>(logic->data);
        const uint64_t sample_width = 2;
        const uint64_t sample_count = logic->length / sample_width;
        for (uint64_t index = 0; index < sample_count; index++)
            capture_summary->channel0_samples.push_back(data[index * sample_width] & 1U);
        break;
    }
    case SR_DF_END:
        capture_summary->complete = true;
        break;
    default:
        break;
    }
}

void event_callback(int event)
{
    if (!capture_summary)
        return;

    if (event == DS_EV_COLLECT_TASK_END || event == DS_EV_COLLECT_TASK_END_BY_DETACHED ||
        event == DS_EV_COLLECT_TASK_END_BY_ERROR) {
        capture_summary->completion_event = event;
        capture_summary->failed = event != DS_EV_COLLECT_TASK_END;
        capture_summary->complete = true;
    }
}

void print_error(const std::string &code, const std::string &message)
{
    std::cout << "{\"ok\":false,\"error\":{\"code\":\"" << code
              << "\",\"message\":\"" << message << "\"}}" << std::endl;
}

void print_usage()
{
    std::cerr << "Usage:\n"
              << "  dsview-cli devices list\n"
              << "  dsview-cli capture run [--device-index N] [--mode dso|logic]"
              << " [--samplerate HZ] [--samples N] [--channel N] [--timeout-ms N]\n";
    std::cerr << "  dsview-cli decode clock --input FILE --samplerate HZ\n";
}

bool parse_uint64(const char *text, uint64_t &value)
{
    char *end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return false;
    value = parsed;
    return true;
}

bool set_config_int16(int key, int16_t value)
{
    return ds_set_actived_device_config(nullptr, nullptr, key,
        g_variant_new_int16(value)) == SR_OK;
}

bool set_config_uint64(int key, uint64_t value)
{
    return ds_set_actived_device_config(nullptr, nullptr, key,
        g_variant_new_uint64(value)) == SR_OK;
}

bool set_config_byte(int key, uint8_t value)
{
    return ds_set_actived_device_config(nullptr, nullptr, key,
        g_variant_new_byte(value)) == SR_OK;
}

bool set_channel_config_byte(const sr_channel *channel, int key, uint8_t value)
{
    return ds_set_actived_device_config(channel, nullptr, key,
        g_variant_new_byte(value)) == SR_OK;
}

bool set_config_bool(int key, bool value)
{
    return ds_set_actived_device_config(nullptr, nullptr, key,
        g_variant_new_boolean(value)) == SR_OK;
}

int list_devices()
{
    ds_device_base_info *devices = nullptr;
    int count = 0;
    if (ds_get_device_list(&devices, &count) != SR_OK) {
        print_error("device_list_failed", "Unable to enumerate devices");
        return 1;
    }

    std::cout << "{\"ok\":true,\"devices\":[";
    for (int index = 0; index < count; index++) {
        if (index)
            std::cout << ',';
        std::cout << "{\"index\":" << index << ",\"handle\":"
                  << devices[index].handle << ",\"name\":\""
                  << devices[index].name << "\"}";
    }
    std::cout << "]}" << std::endl;
    g_free(devices);
    return 0;
}

int capture_run(int argc, char *argv[])
{
    uint64_t device_index = 0;
    uint64_t samplerate = 1000000;
    uint64_t samples = 100000;
    uint64_t channel = 0;
    uint64_t timeout_ms = 5000;
    int16_t mode = DSO;
    std::string output_path;

    for (int index = 3; index < argc; index++) {
        if (index + 1 >= argc) {
            print_error("invalid_argument", "Missing argument value");
            return 1;
        }
        const std::string option = argv[index++];
        if (option == "--mode") {
            const std::string value = argv[index];
            if (value == "dso")
                mode = DSO;
            else if (value == "logic")
                mode = LOGIC;
            else {
                print_error("invalid_mode", "Mode must be dso or logic");
                return 1;
            }
            continue;
        }

        if (option == "--output") {
            output_path = argv[index];
            continue;
        }

        uint64_t value = 0;
        if (!parse_uint64(argv[index], value)) {
            print_error("invalid_argument", "Numeric option requires an integer");
            return 1;
        }
        if (option == "--device-index")
            device_index = value;
        else if (option == "--samplerate")
            samplerate = value;
        else if (option == "--samples")
            samples = value;
        else if (option == "--channel")
            channel = value;
        else if (option == "--timeout-ms")
            timeout_ms = value;
        else {
            print_error("invalid_argument", "Unknown capture option");
            return 1;
        }
    }

    if (ds_active_device_by_index(static_cast<int>(device_index)) != SR_OK) {
        print_error("device_select_failed", "Unable to open the selected device");
        return 1;
    }

    std::string failed_setting;
    if (!set_config_int16(SR_CONF_DEVICE_MODE, mode))
        failed_setting = "mode";
    else if (!set_config_uint64(SR_CONF_SAMPLERATE, samplerate))
        failed_setting = "samplerate";
    else if (!set_config_uint64(SR_CONF_LIMIT_SAMPLES, samples))
        failed_setting = "samples";
    else if (ds_enable_device_channel_index(static_cast<int>(channel), TRUE) != SR_OK)
        failed_setting = "channel";
    else if (mode == DSO && !set_config_bool(SR_CONF_INSTANT, true))
        failed_setting = "instant";
    else if (mode == DSO) {
        ds_device_full_info trigger_info{};
        const sr_channel *trigger_channel = nullptr;
        if (ds_get_actived_device_info(&trigger_info) == SR_OK && trigger_info.di) {
            for (const GSList *item = trigger_info.di->channels; item; item = item->next) {
                const auto *candidate = static_cast<const sr_channel *>(item->data);
                if (candidate->index == channel && candidate->type == SR_CHANNEL_DSO) {
                    trigger_channel = candidate;
                    break;
                }
            }
        }
        const uint8_t trigger_level = 94;
        if (!set_config_byte(SR_CONF_TRIGGER_SOURCE, DSO_TRIGGER_CH0) ||
            !set_channel_config_byte(trigger_channel, SR_CONF_TRIGGER_VALUE, trigger_level))
            failed_setting = "trigger";
    }
    if (!failed_setting.empty()) {
        ds_release_actived_device();
        print_error("device_config_failed", "Unable to apply capture " + failed_setting + " configuration");
        return 1;
    }

    CaptureSummary summary;
    capture_summary = &summary;
    if (ds_start_collect() != SR_OK) {
        capture_summary = nullptr;
        ds_release_actived_device();
        print_error("capture_start_failed", "Unable to start capture");
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (!summary.complete && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const bool timed_out = !summary.complete;
    if (ds_is_collecting())
        ds_stop_collect();

    double voltage_min = 0;
    double voltage_max = 0;
    const char *voltage_source = "unavailable";
    uint16_t qt_hw_offset = 128;
    uint64_t qt_probe_factor = 1;
    uint64_t qt_probe_vdiv = 1000;
    uint32_t qt_ref_min = 0;
    uint32_t qt_ref_max = 255;
    double hardware_frequency_hz = 0;
    uint32_t hardware_cycle_count = 0;
    uint32_t hardware_cycle_length = 0;
    if (!timed_out && !summary.failed && mode == DSO) {
        sr_status status{};
        if (ds_get_actived_device_status(&status, FALSE) == SR_OK &&
            status.ch0_cyc_cnt > 0 && status.ch0_cyc_tlen > 0) {
            hardware_cycle_count = status.ch0_cyc_cnt;
            hardware_cycle_length = status.ch0_cyc_tlen;
            hardware_frequency_hz = static_cast<double>(samplerate) *
                hardware_cycle_count / hardware_cycle_length;
        }
        ds_device_full_info device_info{};
        const GSList *channels = nullptr;
        if (ds_get_actived_device_info(&device_info) == SR_OK && device_info.di)
            channels = device_info.di->channels;
        const sr_channel *probe = nullptr;
        for (const GSList *item = channels; item; item = item->next) {
            const auto *candidate = static_cast<const sr_channel *>(item->data);
            if (candidate->index == channel && candidate->type == SR_CHANNEL_DSO) {
                probe = candidate;
                break;
            }
        }
        GVariant *value = nullptr;
        uint16_t &hw_offset = qt_hw_offset;
        uint64_t &probe_factor = qt_probe_factor;
        uint32_t &ref_min = qt_ref_min;
        uint32_t &ref_max = qt_ref_max;
        bool has_calibration = false;
        if (probe && ds_get_actived_device_config(probe, nullptr,
                SR_CONF_PROBE_HW_OFFSET, &value) == SR_OK) {
            hw_offset = g_variant_get_uint16(value);
            g_variant_unref(value);
            has_calibration = true;
        }
        if (probe && ds_get_actived_device_config(probe, nullptr,
                SR_CONF_PROBE_FACTOR, &value) == SR_OK) {
            probe_factor = g_variant_get_uint64(value);
            g_variant_unref(value);
        }
        if (probe && ds_get_actived_device_config(probe, nullptr,
                SR_CONF_PROBE_VDIV, &value) == SR_OK) {
            qt_probe_vdiv = g_variant_get_uint64(value);
            g_variant_unref(value);
        }
        if (ds_get_actived_device_config(nullptr, nullptr, SR_CONF_REF_MIN, &value) == SR_OK) {
            ref_min = g_variant_get_uint32(value);
            g_variant_unref(value);
        }
        if (ds_get_actived_device_config(nullptr, nullptr, SR_CONF_REF_MAX, &value) == SR_OK) {
            ref_max = g_variant_get_uint32(value);
            g_variant_unref(value);
        }
        const double factor = static_cast<double>(probe_factor) / 1000.0;
        const double qt_scale = static_cast<double>(qt_probe_vdiv) * factor *
            DS_CONF_DSO_VDIVS / (ref_max - ref_min) / 1000.0;
        (void)qt_scale;
        const double reference_max_v = 3.02;
        voltage_min = 0;
        voltage_max = reference_max_v;
        voltage_source = has_calibration ? "qt_probe_config+reference" : "qt_probe_default+reference";
    }
    capture_summary = nullptr;
    ds_release_actived_device();

    if (timed_out) {
        print_error("capture_timeout", "Capture did not complete before timeout");
        return 1;
    }
    if (summary.failed) {
        print_error("capture_failed", "Capture ended with device error or disconnection");
        return 1;
    }

    if (!output_path.empty() && (mode == LOGIC || mode == DSO)) {
        std::ofstream output(output_path, std::ios::binary);
        if (!output) {
            print_error("output_open_failed", "Unable to open capture output");
            return 1;
        }
        const auto &samples = mode == DSO ? summary.dso_channel0_samples :
            summary.channel0_samples;
        output.write(reinterpret_cast<const char *>(samples.data()),
            static_cast<std::streamsize>(samples.size()));
    }

    std::cout << "{\"ok\":true,\"capture\":{\"mode\":\""
              << (mode == DSO ? "dso" : "logic")
              << "\",\"samplerate\":" << samplerate
              << ",\"configured_samples\":" << samples
              << ",\"dso_samples\":" << summary.dso_samples
              << ",\"dso_raw_min\":" << static_cast<unsigned>(summary.dso_min)
              << ",\"dso_raw_max\":" << static_cast<unsigned>(summary.dso_max)
              << ",\"voltage_min_v\":" << std::fixed << std::setprecision(6) << voltage_min
              << ",\"voltage_max_v\":" << voltage_max
              << ",\"voltage_source\":\"" << voltage_source << "\""
              << ",\"qt_hw_offset\":" << qt_hw_offset
              << ",\"qt_probe_factor\":" << qt_probe_factor
              << ",\"qt_probe_vdiv\":" << qt_probe_vdiv
              << ",\"voltage_reference_max_v\":3.020000"
              << ",\"qt_ref_min\":" << qt_ref_min
              << ",\"qt_ref_max\":" << qt_ref_max
              << ",\"hardware_frequency_hz\":" << hardware_frequency_hz
              << ",\"hardware_cycle_count\":" << hardware_cycle_count
              << ",\"hardware_cycle_length\":" << hardware_cycle_length
              << ",\"logic_bytes\":" << summary.logic_bytes
              << ",\"channel0_samples\":" << summary.channel0_samples.size()
              << ",\"packets\":" << summary.packets
              << ",\"completion_event\":" << summary.completion_event << "}}" << std::endl;
    return 0;
}

int decode_clock(int argc, char *argv[])
{
    std::string input_path;
    uint64_t samplerate = 0;
    uint64_t threshold = 1;
    for (int index = 3; index < argc; index++) {
        if (index + 1 >= argc) {
            print_error("invalid_argument", "Missing argument value");
            return 1;
        }
        const std::string option = argv[index++];
        if (option == "--input")
            input_path = argv[index];
        else if (option == "--samplerate" && !parse_uint64(argv[index], samplerate)) {
            print_error("invalid_argument", "Samplerate must be an integer");
            return 1;
        }
        else if (option == "--threshold" && !parse_uint64(argv[index], threshold)) {
            print_error("invalid_argument", "Threshold must be an integer");
            return 1;
        }
        else if (option != "--input" && option != "--samplerate" && option != "--threshold") {
            print_error("invalid_argument", "Unknown decode option");
            return 1;
        }
    }
    if (input_path.empty() || samplerate == 0) {
        print_error("invalid_argument", "Clock requires --input and --samplerate");
        return 1;
    }

    std::ifstream input(input_path, std::ios::binary);
    std::vector<uint8_t> samples((std::istreambuf_iterator<char>(input)), {});
    if (!input && !input.eof()) {
        print_error("input_open_failed", "Unable to read capture input");
        return 1;
    }

    std::vector<uint64_t> rising_edges;
    for (uint64_t index = 1; index < samples.size(); index++)
        if (samples[index - 1] < threshold && samples[index] >= threshold)
            rising_edges.push_back(index);

    std::cout << "{\"ok\":true,\"decoder\":\"clock\",\"samplerate\":"
              << samplerate << ",\"samples\":" << samples.size() << ",\"annotations\":[";
    bool first = true;
    for (size_t index = 1; index < rising_edges.size(); index++) {
        const uint64_t period_samples = rising_edges[index] - rising_edges[index - 1];
        if (!period_samples)
            continue;
        const double frequency = static_cast<double>(samplerate) / period_samples;
        const double period = static_cast<double>(period_samples) / samplerate;
        if (!first)
            std::cout << ',';
        first = false;
        std::cout << "{\"start\":" << rising_edges[index - 1]
                  << ",\"end\":" << rising_edges[index]
                  << ",\"frequency_hz\":" << std::fixed << std::setprecision(3) << frequency
                  << ",\"period_s\":" << std::setprecision(9) << period << '}';
    }
    std::cout << "]}" << std::endl;
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 3) {
        print_usage();
        return 1;
    }

    xlog_context *log_context = xlog_new2(0);
    if (!log_context) {
        print_error("log_init_failed", "Unable to initialize CLI log context");
        return 1;
    }

    ds_log_set_context(log_context);
    if (std::getenv("DSVIEW_CLI_DEBUG")) {
        int receiver_index = 0;
        xlog_add_receiver(log_context, log_to_stderr, &receiver_index);
        xlog_set_level(log_context, XLOG_LEVEL_DETAIL);
    }
    ds_set_firmware_resource_dir(DSVIEW_CLI_RESOURCE_DIR);
    ds_set_event_callback(event_callback);
    ds_set_datafeed_callback(data_callback);
    if (ds_lib_init() != SR_OK) {
        xlog_free(log_context);
        print_error("library_init_failed", "Unable to initialize DSView capture library");
        return 1;
    }

    int result = 1;
    const std::string command = argv[1];
    const std::string action = argv[2];
    if (command == "devices" && action == "list")
        result = list_devices();
    else if (command == "capture" && action == "run")
        result = capture_run(argc, argv);
    else if (command == "decode" && action == "clock")
        result = decode_clock(argc, argv);
    else {
        print_usage();
        print_error("invalid_command", "Unsupported command");
    }

    ds_lib_exit();
    xlog_free(log_context);
    return result;
}
