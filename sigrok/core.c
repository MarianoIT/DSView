/* DSL driver lifecycle and event sources, hosted by upstream libsigrok. */
#include "core.h"
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"
#include <string.h>

/* Vendor packet payloads stay opaque; no DSL structure is cast to a sigrok ABI. */
#define DSL_VENDOR_PACKET 0x8000
struct dsl_core {
    struct sr_context *context;
    struct sr_session *session;
    GHashTable *devices;
    GSList *drivers;
    GSList *sources;
    gint stop_requested;
};
struct dsl_core_driver {
    struct sr_dev_driver driver;
    struct dsl_core *core;
    void *vendor_driver;
    struct dsl_core_ops ops;
    GSList *devices;
};
struct device_binding {
    struct dsl_core_driver *driver;
    void *vendor_device;
};
struct source_binding {
    dsl_core_source_cb callback;
    const void *data;
};
struct packet_envelope {
    const void *device;
    const void *packet;
    dsl_core_data_cb callback;
};
static struct dsl_core_driver *driver_data(const struct sr_dev_driver *driver)
{
    return (struct dsl_core_driver *)driver; /* driver is the first member */
}
static int driver_init(struct sr_dev_driver *driver, struct sr_context *context)
{
    struct dsl_core_driver *binding = driver_data(driver);
    driver->context = binding;
    return binding->ops.init(binding->vendor_driver, context->libusb_ctx) == 0 ? SR_OK : SR_ERR;
}
static int driver_cleanup(const struct sr_dev_driver *driver)
{
    struct dsl_core_driver *binding = driver_data(driver);
    return binding->ops.cleanup(binding->vendor_driver) == 0 ? SR_OK : SR_ERR;
}
static GSList *driver_list(const struct sr_dev_driver *driver)
{
    return driver_data(driver)->devices;
}
static int driver_clear(const struct sr_dev_driver *driver)
{
    /* Vendor device lifetimes belong to DSView, including deferred hotplug removal. */
    return driver_data(driver)->devices ? SR_ERR : SR_OK;
}
static int driver_open(struct sr_dev_inst *device)
{
    struct device_binding *binding = device->priv;
    int ret = binding->driver->ops.open(binding->vendor_device);
    if (ret == SR_OK) device->status = SR_ST_ACTIVE;
    return ret == 0 ? SR_OK : SR_ERR;
}
static int driver_close(struct sr_dev_inst *device)
{
    struct device_binding *binding = device->priv;
    int ret = binding->driver->ops.close(binding->vendor_device);
    if (ret == SR_OK) device->status = SR_ST_INACTIVE;
    return ret == 0 ? SR_OK : SR_ERR;
}
static int driver_start(const struct sr_dev_inst *device)
{
    struct device_binding *binding = device->priv;
    return binding->driver->ops.start(binding->vendor_device) == 0 ? SR_OK : SR_ERR;
}
static int driver_stop(struct sr_dev_inst *device)
{
    struct device_binding *binding = device->priv;
    return binding->driver->ops.stop(binding->vendor_device) == 0 ? SR_OK : SR_ERR;
}
static void data_callback(const struct sr_dev_inst *device,
    const struct sr_datafeed_packet *packet, void *data)
{
    (void)device;
    (void)data;
    if (packet->type == DSL_VENDOR_PACKET) {
        const struct packet_envelope *envelope = packet->payload;
        envelope->callback(envelope->device, envelope->packet);
    }
}
static GSList *driver_scan(struct sr_dev_driver *driver, GSList *options)
{
    struct dsl_core_driver *binding = driver_data(driver);
    GSList *vendor_devices = binding->ops.scan(binding->vendor_driver, options);
    GSList *result = NULL;
    for (GSList *item = vendor_devices; item; item = item->next) {
        if (dsl_core_attach(binding->core, binding, item->data) == SR_OK)
            result = g_slist_append(result, g_hash_table_lookup(binding->core->devices, item->data));
    }
    g_slist_free(vendor_devices);
    return result;
}
struct dsl_core *dsl_core_new(void)
{
    struct dsl_core *core = g_new0(struct dsl_core, 1);
    if (sr_init(&core->context) != SR_OK) { g_free(core); return NULL; }
    core->devices = g_hash_table_new(g_direct_hash, g_direct_equal);
    return core;
}
void *dsl_core_usb_context(struct dsl_core *core)
{
    return core ? core->context->libusb_ctx : NULL;
}
struct dsl_core_driver *dsl_core_driver_new(struct dsl_core *core,
    const char *name, const char *description, void *driver,
    const struct dsl_core_ops *ops)
{
    struct dsl_core_driver *binding = g_new0(struct dsl_core_driver, 1);
    binding->core = core;
    binding->vendor_driver = driver;
    binding->ops = *ops;
    binding->driver = (struct sr_dev_driver) {
        .name = name, .longname = description, .api_version = 1,
        .init = driver_init, .cleanup = driver_cleanup, .scan = driver_scan,
        .dev_list = driver_list, .dev_clear = driver_clear,
        .dev_open = driver_open, .dev_close = driver_close,
        .dev_acquisition_start = driver_start, .dev_acquisition_stop = driver_stop,
    };
    if (sr_driver_init(core->context, &binding->driver) != SR_OK) {
        g_free(binding);
        return NULL;
    }
    size_t count = 0;
    while (core->context->driver_list[count]) count++;
    core->context->driver_list = g_realloc_n(core->context->driver_list,
        count + 2, sizeof(struct sr_dev_driver *));
    core->context->driver_list[count] = &binding->driver;
    core->context->driver_list[count + 1] = NULL;
    core->drivers = g_slist_append(core->drivers, binding);
    return binding;
}
int dsl_core_attach(struct dsl_core *core, struct dsl_core_driver *driver, void *vendor_device)
{
    if (g_hash_table_contains(core->devices, vendor_device)) return SR_OK;
    const char *vendor = NULL, *model = NULL;
    driver->ops.identity(vendor_device, &vendor, &model);
    struct sr_dev_inst *device = sr_dev_inst_user_new(vendor, model, NULL);
    device->driver = &driver->driver;
    device->inst_type = SR_INST_USB;
    device->status = SR_ST_INACTIVE;
    struct device_binding *binding = g_new0(struct device_binding, 1);
    binding->driver = driver;
    binding->vendor_device = vendor_device;
    device->priv = binding;
    unsigned count = driver->ops.channels(vendor_device);
    for (unsigned i = 0; i < count; i++) {
        char name[32];
        g_snprintf(name, sizeof(name), "%u", i);
        sr_channel_new(device, i, driver->ops.channel_is_logic(vendor_device, i) ? SR_CHANNEL_LOGIC : SR_CHANNEL_ANALOG,
            driver->ops.channel_enabled(vendor_device, i), name);
    }
    g_hash_table_insert(core->devices, vendor_device, device);
    driver->devices = g_slist_append(driver->devices, device);
    return SR_OK;
}
GSList *dsl_core_scan(struct dsl_core_driver *driver, GSList *options)
{
    GSList *devices = sr_driver_scan(&driver->driver, options);
    GSList *result = NULL;
    for (GSList *item = devices; item; item = item->next) {
        struct device_binding *binding = ((struct sr_dev_inst *)item->data)->priv;
        result = g_slist_append(result, binding->vendor_device);
    }
    g_slist_free(devices);
    return result;
}
int dsl_core_open(struct dsl_core *core, void *device)
{
    struct sr_dev_inst *native = core ? g_hash_table_lookup(core->devices, device) : NULL;
    return native ? sr_dev_open(native) : SR_ERR_ARG;
}
int dsl_core_close(struct dsl_core *core, void *device)
{
    struct sr_dev_inst *native = core ? g_hash_table_lookup(core->devices, device) : NULL;
    return native ? (native->status == SR_ST_ACTIVE ? sr_dev_close(native) : SR_OK) : SR_ERR_ARG;
}
void dsl_core_forget(struct dsl_core *core, void *device)
{
    struct sr_dev_inst *native = core ? g_hash_table_lookup(core->devices, device) : NULL;
    if (!native) return;
    struct device_binding *binding = native->priv;
    binding->driver->devices = g_slist_remove(binding->driver->devices, native);
    g_hash_table_remove(core->devices, device);
    g_free(binding);
    sr_dev_inst_free(native);
}
int dsl_core_session_new(struct dsl_core *core)
{
    if (!core) return SR_ERR_ARG;
    dsl_core_session_destroy(core);
    g_atomic_int_set(&core->stop_requested, 0);
    int ret = sr_session_new(core->context, &core->session);
    if (ret != SR_OK) return ret;
    return sr_session_datafeed_callback_add(core->session, data_callback, NULL);
}
int dsl_core_session_start(struct dsl_core *core, void *device)
{
    struct sr_dev_inst *native = g_hash_table_lookup(core->devices, device);
    if (!native || !core->session) return SR_ERR_ARG;
    struct device_binding *binding = native->priv;
    g_slist_free_full(native->channels, (GDestroyNotify)sr_channel_free);
    native->channels = NULL;
    unsigned count = binding->driver->ops.channels(device);
    for (unsigned index = 0; index < count; index++) {
        char name[32];
        g_snprintf(name, sizeof(name), "%u", index);
        sr_channel_new(native, index, binding->driver->ops.channel_is_logic(device, index) ? SR_CHANNEL_LOGIC : SR_CHANNEL_ANALOG,
            binding->driver->ops.channel_enabled(device, index), name);
    }
    int ret = sr_session_dev_add(core->session, native);
    if (ret == SR_OK) ret = sr_session_start(core->session);
    /* A stop arriving before upstream creates its main context must survive. */
    if (ret == SR_OK && g_atomic_int_get(&core->stop_requested)) sr_session_stop(core->session);
    return ret;
}
int dsl_core_session_run(struct dsl_core *core)
{
    return core && core->session ? sr_session_run(core->session) : SR_ERR_ARG;
}
int dsl_core_session_stop(struct dsl_core *core)
{
    if (!core || !core->session) return SR_ERR_ARG;
    g_atomic_int_set(&core->stop_requested, 1);
    return sr_session_stop(core->session);
}
void dsl_core_session_destroy(struct dsl_core *core)
{
    if (!core || !core->session) return;
    sr_session_destroy(core->session);
    core->session = NULL;
    g_slist_free_full(core->sources, g_free);
    core->sources = NULL;
}
static int source_callback(int fd, int events, void *data)
{
    struct source_binding *binding = data;
    return binding->callback(fd, events, binding->data);
}
int dsl_core_source_add(struct dsl_core *core, int fd, int events, int timeout,
    dsl_core_source_cb callback, const void *data)
{
    if (!core || !core->session || !callback) return SR_ERR_ARG;
    struct source_binding *binding = g_new0(struct source_binding, 1);
    binding->callback = callback;
    binding->data = data;
    int ret = sr_session_source_add(core->session, fd, events, timeout, source_callback, binding);
    if (ret == SR_OK) core->sources = g_slist_prepend(core->sources, binding);
    else g_free(binding);
    return ret;
}
int dsl_core_source_remove(struct dsl_core *core, int fd)
{
    return core && core->session ? sr_session_source_remove(core->session, fd) : SR_ERR_ARG;
}
int dsl_core_send(struct dsl_core *core, const void *device,
    const void *packet, dsl_core_data_cb callback)
{
    struct sr_dev_inst *native = core ? g_hash_table_lookup(core->devices, device) : NULL;
    if (!native || !core->session || !packet || !callback) return SR_ERR_ARG;
    const struct packet_envelope envelope = {device, packet, callback};
    const struct sr_datafeed_packet native_packet = {DSL_VENDOR_PACKET, &envelope};
    return sr_session_send(native, &native_packet);
}
void dsl_core_free(struct dsl_core *core)
{
    if (!core) return;
    dsl_core_session_destroy(core);
    GList *devices = g_hash_table_get_keys(core->devices);
    for (GList *item = devices; item; item = item->next) dsl_core_forget(core, item->data);
    g_list_free(devices);
    sr_exit(core->context);
    g_hash_table_destroy(core->devices);
    g_slist_free_full(core->drivers, g_free);
    g_free(core);
}
const char *dsl_core_version(void)
{
    return sr_package_version_string_get();
}
