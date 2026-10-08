#pragma once
#include <stdint.h>

// ============================================================================
// Config polling
//
// A status frame (getDevSta) carries live values but not the stored
// configuration: schedules, cycle timings, PPFD windows and mode settings
// are missing. Without asking for them explicitly the corresponding Home
// Assistant entities stay empty until the user happens to change
// something in the Spider Farmer app.
//
// So the bridge asks: getConfigField for light, light2, fan and blower,
// once right after a controller connects and then on a slow timer. The
// replies arrive as ordinary UP frames and feed the same caches and
// normaliser as everything else.
// ============================================================================

// Asks the poll task to send a round now.
//
// Deliberately asynchronous: the requests are spaced half a second apart
// so the controller is not hit with a burst, and doing that inline would
// block the relay loop for two seconds — long enough to stall the
// session it is supposed to serve.
void config_poll_request(void);

// Asks only for the calibration block.
//
// Safe with cloud mirroring on, where the full round is not: the
// session loop that motivated that restriction came from a burst of
// requests, and a single one is answered without disturbing the cloud.
// Needed because calibration never appears in a status frame.
void config_poll_request_calibration(void);

// Starts the poll task. Safe to call more than once.
void config_poll_start(void);

// Runs fn every every_s seconds on the poll task (which wakes each second
// anyway), instead of a dedicated task that only sleeps. Up to 4 hooks.
// fn must be quick and must not block for long.
typedef void (*config_poll_hook_t)(void);
void config_poll_add_hook(config_poll_hook_t fn, uint32_t every_s);

// Tells the poller which session to address. Pass NULL when the session
// ends so the timer stops sending into a closed connection.
void config_poll_set_session(const char *mac, const char *uid);
