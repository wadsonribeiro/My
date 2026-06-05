#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* Defaults */
#define DC_DEFAULT_FPS      60
#define DC_DEFAULT_RES      1080
#define DC_DEFAULT_BITRATE  "8M"

/* Supported options */
#define DC_FPS_COUNT      4
#define DC_RES_COUNT      4
#define DC_BITRATE_COUNT  4

static const int    DC_FPS_OPTIONS[DC_FPS_COUNT]     = {30, 60, 120, 240};
static const int    DC_RES_OPTIONS[DC_RES_COUNT]     = {540, 720, 1080, 1440};
static const char * DC_BITRATE_OPTIONS[DC_BITRATE_COUNT] = {"4M","8M","16M","32M"};

typedef struct {
    char    device_id[64];   /* ADB serial / IP:port */
    char    device_name[64]; /* friendly name         */
    int     fps;
    int     resolution;
    char    bitrate[8];
    bool    audio;           /* forward audio to PC?  */
} sc_device_config;

/* Initialise with defaults */
void sc_device_config_init(sc_device_config *cfg, const char *device_id);

/* Load/save devices_data.json  (located next to the executable) */
bool sc_device_config_load(sc_device_config *cfg, const char *device_id);
bool sc_device_config_save(const sc_device_config *cfg);

/* Build the scrcpy command line from a config */
void sc_device_config_build_cmd(const sc_device_config *cfg,
                                char *out, size_t out_len);

#endif /* DEVICE_CONFIG_H */
