/* Integration test: real DSL device, repeated sessions and cancellation. */
#include <libsigrok.h>
#include "libsigrok-internal.h"
#include "../sigrok/core.h"
#include <stdio.h>
#include <string.h>

static gint ended, failed;
static uint64_t received;
static unsigned iterations = 20;
static gboolean instant = TRUE;
static void data_received(const struct sr_dev_inst *device, const struct sr_datafeed_packet *packet)
{
    (void)device;
    if (packet->status != SR_PKT_OK) g_atomic_int_set(&failed, 1);
    if (packet->type == SR_DF_DSO && packet->status == SR_PKT_OK) {
        const struct sr_datafeed_dso *data = packet->payload;
        if (!data || !data->data || data->num_samples < 0) {
            g_atomic_int_set(&failed, 1);
            return;
        }
        if (data->num_samples > 10000) g_atomic_int_set(&failed, 1);
        received += data->num_samples;
        /* Touch the entire advertised data range under AddressSanitizer. */
        const uint8_t *samples = data->data;
        volatile unsigned checksum = 0;
        size_t channels = 0;
        for (GSList *entry = data->probes; entry; entry = entry->next)
            if (((struct sr_channel *)entry->data)->enabled) channels++;
        for (size_t i = 0; i < (size_t)data->num_samples * channels; i++) checksum += samples[i];
        (void)checksum;
        if (!instant && received >= 10000) g_atomic_int_set(&ended, 1);
    }
    if (packet->type == SR_DF_END) g_atomic_int_set(&ended, 1);
}
static void event_received(int event)
{
    if (event == DS_EV_COLLECT_TASK_END_BY_ERROR || event == DS_EV_COLLECT_TASK_END_BY_DETACHED) {
        g_atomic_int_set(&failed, 1);
        g_atomic_int_set(&ended, 1);
    }
}
static void log_received(const char *data, int length) { fwrite(data, 1, length, stderr); }
static int set(int key, GVariant *value)
{
    return ds_set_actived_device_config(NULL, NULL, key, value);
}
int main(int argc, char **argv)
{
    if (argc == 2) iterations = (unsigned)g_ascii_strtoull(argv[1], NULL, 10);
    if (!iterations || iterations > 1000) return 2;
    xlog_context *log = xlog_new2(0);
    if (!log) return 1;
    ds_log_set_context(log);
    if (g_getenv("DSVIEW_CLI_DEBUG")) {
        int receiver;
        xlog_add_receiver(log, log_received, &receiver);
        xlog_set_level(log, XLOG_LEVEL_DETAIL);
    }
    ds_set_firmware_resource_dir(DSVIEW_CLI_RESOURCE_DIR);
    ds_set_event_callback(event_received);
    ds_set_datafeed_callback(data_received);
    if (ds_lib_init() != SR_OK) { xlog_free(log); return 1; }
    int result = 1;
    struct ds_device_base_info *devices = NULL;
    int count = 0;
    if (ds_get_device_list(&devices, &count) != SR_OK || !count) goto done;
    ds_device_handle handle = devices[0].handle;
    printf("core=%s device=%s iterations=%u\n", dsl_core_version(), devices[0].name, iterations);
    for (unsigned i = 0; i < iterations; i++) {
        if (ds_active_device(handle) != SR_OK) goto done;
        struct ds_device_full_info info;
        if (ds_get_actived_device_info(&info) != SR_OK || info.dev_type != DEV_TYPE_USB) goto done;
        instant = i % 3 != 2;
        if (set(SR_CONF_DEVICE_MODE, g_variant_new_int16(DSO)) != SR_OK ||
            set(SR_CONF_INSTANT, g_variant_new_boolean(instant)) != SR_OK ||
            set(SR_CONF_SAMPLERATE, g_variant_new_uint64(1000000)) != SR_OK ||
            set(SR_CONF_LIMIT_SAMPLES, g_variant_new_uint64(10000)) != SR_OK ||
            set(SR_CONF_TRIGGER_SOURCE, g_variant_new_byte(DSO_TRIGGER_AUTO)) != SR_OK) goto done;
        if (ds_enable_device_channel_index(0, TRUE) != SR_OK ||
            ds_enable_device_channel_index(1, i % 2 == 0) != SR_OK) goto done;
        /* Enabling a channel may reset the device sample limit. */
        if (set(SR_CONF_LIMIT_SAMPLES, g_variant_new_uint64(10000)) != SR_OK) goto done;
        g_atomic_int_set(&ended, 0);
        g_atomic_int_set(&failed, 0);
        received = 0;
        if (ds_start_collect() != SR_OK) goto done;
        int64_t deadline = g_get_monotonic_time() + 10000000;
        while (!g_atomic_int_get(&ended) && g_get_monotonic_time() < deadline) g_usleep(1000);
        ds_stop_collect(); /* Joins the worker even after natural completion. */
        if (!g_atomic_int_get(&ended) || g_atomic_int_get(&failed) || (instant ? received != 10000 : received < 10000)) {
            fprintf(stderr, "capture %u failed: end=%d error=%d samples=%llu\n", i,
                ended, failed, (unsigned long long)received);
            goto done;
        }
        uint64_t captured = received;
        /* Start another acquisition and cancel while transfers are outstanding. */
        g_atomic_int_set(&ended, 0);
        if (ds_start_collect() != SR_OK) goto done;
        if (i % 2) g_usleep(1000); /* Also exercise cancellation before start. */
        ds_stop_collect();
        if (ds_is_collecting()) goto done;
        if (ds_release_actived_device() != SR_OK) goto done;
        printf("cycle=%u mode=%s channels=%u captured=%llu cancellation=ok\n",
            i + 1, instant ? "instant" : "continuous", i % 2 ? 1 : 2, (unsigned long long)captured);
        fflush(stdout);
    }
    result = 0;
done:
    g_free(devices);
    ds_lib_exit();
    xlog_free(log);
    return result;
}
