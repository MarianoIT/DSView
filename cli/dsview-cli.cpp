#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <glib.h>
#include <log/xlog.h>

extern "C" {
#include <libsigrok.h>
}

namespace {

struct CaptureSummary {
    std::atomic<bool> complete{false};
    std::atomic<bool> failed{false};
    std::atomic<int> completion_event{0};
    std::atomic<uint64_t> dso_samples{0};
    std::atomic<uint64_t> logic_bytes{0};
    std::atomic<uint64_t> packets{0};
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
        capture_summary->dso_samples +=
            static_cast<const sr_datafeed_dso *>(packet->payload)->num_samples;
        break;
    case SR_DF_LOGIC:
        capture_summary->logic_bytes +=
            static_cast<const sr_datafeed_logic *>(packet->payload)->length;
        break;
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
    else if (mode == DSO && !set_config_byte(SR_CONF_TRIGGER_SOURCE, DSO_TRIGGER_AUTO))
        failed_setting = "trigger";
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

    std::cout << "{\"ok\":true,\"capture\":{\"mode\":\""
              << (mode == DSO ? "dso" : "logic")
              << "\",\"samplerate\":" << samplerate
              << ",\"configured_samples\":" << samples
              << ",\"dso_samples\":" << summary.dso_samples
              << ",\"logic_bytes\":" << summary.logic_bytes
              << ",\"packets\":" << summary.packets
              << ",\"completion_event\":" << summary.completion_event << "}}" << std::endl;
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
    else {
        print_usage();
        print_error("invalid_command", "Unsupported command");
    }

    ds_lib_exit();
    xlog_free(log_context);
    return result;
}
