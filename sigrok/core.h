/* Opaque boundary between DSL protocol types and the upstream sigrok ABI. */
#ifndef DSVIEW_SIGROK_CORE_H
#define DSVIEW_SIGROK_CORE_H
#include <glib.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct dsl_core;
struct dsl_core_driver;
struct dsl_core_ops {
    int (*init)(void *driver, void *usb_context);
    int (*cleanup)(void *driver);
    GSList *(*scan)(void *driver, GSList *options);
    void (*identity)(void *device, const char **vendor, const char **model);
    unsigned (*channels)(void *device);
    int (*channel_enabled)(void *device, unsigned channel);
    int (*channel_is_logic)(void *device, unsigned channel);
    int (*open)(void *device);
    int (*close)(void *device);
    int (*start)(void *device);
    int (*stop)(void *device);
};
struct dsl_core *dsl_core_new(void);
void dsl_core_free(struct dsl_core *core);
void *dsl_core_usb_context(struct dsl_core *core);
struct dsl_core_driver *dsl_core_driver_new(struct dsl_core *core,
    const char *name, const char *description, void *driver,
    const struct dsl_core_ops *ops);
GSList *dsl_core_scan(struct dsl_core_driver *driver, GSList *options);
int dsl_core_open(struct dsl_core *core, void *device);
int dsl_core_close(struct dsl_core *core, void *device);
void dsl_core_forget(struct dsl_core *core, void *device);
int dsl_core_attach(struct dsl_core *core, struct dsl_core_driver *driver, void *device);
int dsl_core_session_new(struct dsl_core *core);
int dsl_core_session_start(struct dsl_core *core, void *device);
int dsl_core_session_run(struct dsl_core *core);
int dsl_core_session_stop(struct dsl_core *core);
void dsl_core_session_destroy(struct dsl_core *core);
typedef int (*dsl_core_source_cb)(int fd, int events, const void *data);
int dsl_core_source_add(struct dsl_core *core, int fd, int events, int timeout,
    dsl_core_source_cb callback, const void *data);
int dsl_core_source_remove(struct dsl_core *core, int fd);
typedef void (*dsl_core_data_cb)(const void *device, const void *packet);
int dsl_core_send(struct dsl_core *core, const void *device,
    const void *packet, dsl_core_data_cb callback);
const char *dsl_core_version(void);
#ifdef __cplusplus
}
#endif
#endif
