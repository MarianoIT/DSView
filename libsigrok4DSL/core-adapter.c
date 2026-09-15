/* DSL-facing adapter. The upstream ABI is isolated in sigrok/core.c. */
#include "libsigrok-internal.h"
#include "log.h"
#include "../sigrok/core.h"

static struct dsl_core *core;
static struct sr_context *protocol_context;
static GSList *source_callbacks;
struct callback_binding {
    sr_receive_data_callback_t callback;
    const struct sr_dev_inst *device;
};
static int protocol_init(void *driver, void *usb_context)
{
    struct sr_dev_driver *d = driver;
    protocol_context->libusb_ctx = usb_context;
    return d->init ? d->init(protocol_context) : SR_OK;
}
static int protocol_cleanup(void *driver)
{
    struct sr_dev_driver *d = driver;
    d->core_driver = NULL;
    return d->cleanup ? d->cleanup() : SR_OK;
}
static GSList *protocol_scan(void *driver, GSList *options)
{
    struct sr_dev_driver *d = driver;
    return d->scan ? d->scan(options) : NULL;
}
static void protocol_identity(void *device, const char **vendor, const char **model)
{
    struct sr_dev_inst *d = device;
    *vendor = d->vendor;
    *model = d->name;
}
static unsigned protocol_channels(void *device)
{
    return g_slist_length(((struct sr_dev_inst *)device)->channels);
}
static int protocol_channel_enabled(void *device, unsigned index)
{
    struct sr_channel *ch = g_slist_nth_data(((struct sr_dev_inst *)device)->channels, index);
    return ch && ch->enabled;
}
static int protocol_channel_is_logic(void *device, unsigned index)
{
    struct sr_channel *ch = g_slist_nth_data(((struct sr_dev_inst *)device)->channels, index);
    return ch && ch->type == SR_CHANNEL_LOGIC;
}
static int protocol_open(void *device)
{
    struct sr_dev_inst *d = device;
    return d->driver->dev_open ? d->driver->dev_open(d) : SR_OK;
}
static int protocol_close(void *device)
{
    struct sr_dev_inst *d = device;
    return d->driver->dev_close ? d->driver->dev_close(d) : SR_OK;
}
static int protocol_start(void *device)
{
    struct sr_dev_inst *d = device;
    return d->driver->dev_acquisition_start(d, d);
}
static int protocol_stop(void *device)
{
    struct sr_dev_inst *d = device;
    return d->driver->dev_acquisition_stop(d, d);
}
int ds_core_init_context(struct sr_context *context)
{
    if (core) return SR_ERR_CALL_STATUS;
    core = dsl_core_new();
    if (!core) return SR_ERR;
    protocol_context = context;
    context->libusb_ctx = dsl_core_usb_context(core);
    return SR_OK;
}
void ds_core_exit_context(void)
{
    dsl_core_free(core);
    core = NULL;
    protocol_context = NULL;
}
int ds_core_register_driver(struct sr_dev_driver *driver)
{
    static const struct dsl_core_ops ops = {
        protocol_init, protocol_cleanup, protocol_scan, protocol_identity,
        protocol_channels, protocol_channel_enabled, protocol_channel_is_logic,
        protocol_open, protocol_close, protocol_start, protocol_stop
    };
    if (driver->core_driver) return SR_OK;
    driver->core_driver = dsl_core_driver_new(core, driver->name, driver->longname, driver, &ops);
    return driver->core_driver ? SR_OK : SR_ERR;
}
GSList *ds_core_scan_driver(struct sr_dev_driver *driver, GSList *options)
{
    return driver->core_driver ? dsl_core_scan(driver->core_driver, options) : NULL;
}
int ds_core_open_device(struct sr_dev_inst *device)
{
    if (ds_core_register_driver(device->driver) != SR_OK) return SR_ERR;
    if (dsl_core_attach(core, device->driver->core_driver, device) != SR_OK) return SR_ERR;
    return dsl_core_open(core, device);
}
int ds_core_close_device(struct sr_dev_inst *device)
{
    return dsl_core_close(core, device);
}
void ds_core_forget_device(struct sr_dev_inst *device)
{
    dsl_core_forget(core, device);
}
int ds_core_start_device(struct sr_dev_inst *device)
{
    return dsl_core_session_start(core, device);
}
int ds_core_forward(const struct sr_dev_inst *device,
    const struct sr_datafeed_packet *packet, dsl_core_data_cb callback)
{
    return dsl_core_send(core, device, packet, callback);
}
int sr_session_new(void)
{
    sr_session_destroy();
    return dsl_core_session_new(core) == 0 ? SR_OK : SR_ERR;
}
int sr_session_destroy(void)
{
    dsl_core_session_destroy(core);
    g_slist_free_full(source_callbacks, g_free);
    source_callbacks = NULL;
    return SR_OK;
}
int sr_session_run(void)
{
    int ret = dsl_core_session_run(core);
    current_device_acquisition_stop();
    return ret;
}
int sr_session_stop(void)
{
    return dsl_core_session_stop(core);
}
static int source_callback(int fd, int events, const void *data)
{
    const struct callback_binding *binding = data;
    return binding->callback(fd, events, binding->device);
}
int sr_session_source_add(int fd, int events, int timeout,
    sr_receive_data_callback_t callback, const struct sr_dev_inst *device)
{
    if (!callback) return SR_ERR_ARG;
    struct callback_binding *binding = g_new0(struct callback_binding, 1);
    binding->callback = callback;
    binding->device = device;
    int ret = dsl_core_source_add(core, fd, events, timeout, source_callback, binding);
    if (ret == SR_OK) source_callbacks = g_slist_prepend(source_callbacks, binding);
    else g_free(binding);
    return ret;
}
int sr_session_source_remove(int fd)
{
    return dsl_core_source_remove(core, fd);
}
