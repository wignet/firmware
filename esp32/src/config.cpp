#include "config.h"

// Local server configuration
const int HTTP_PORT = 80;

// Wignet Server Configuration. A bench build may talk to a server on the
// local network instead: PLATFORMIO_BUILD_FLAGS='-DWIGNET_BENCH_HOST=\"192.168.x.y\"
// -DWIGNET_BENCH_PORT=5050' (docs/scenes.md, "On the bench").
#if defined(WIGNET_BENCH_HOST)
const char *WIGNET_HOST = WIGNET_BENCH_HOST;
const int WIGNET_PORT = WIGNET_BENCH_PORT;
#else
const char *WIGNET_HOST = "api.wignet.app";
const int WIGNET_PORT = 80;
#endif
const char *WIGNET_IMAGE_PATH = "/pic/4displayB5.bmp";
const int WIGNET_POLL_INTERVAL_SEC = 60;
const int WIGNET_INITIAL_POLL_DELAY_SEC = 30;

// Deep sleep configuration
const int DEFAULT_REFRESH_SEC = 30; // Fallback if server unreachable
const unsigned long SAFE_PERIOD_MS =
    1 * 60 * 1000; // 1 minute before deep sleep allowed
const char *WIGNET_SCHEDULE_PATH = "/schedule"; // Server schedule endpoint
