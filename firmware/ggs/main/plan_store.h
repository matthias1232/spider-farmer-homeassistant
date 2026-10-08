#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// Long grow plans, kept on the bridge
//
// DORMANT while SB_LONG_PLAN is 0 (sb_config.h): the grow plan lives on the
// controller only. Rule: the bridge never sends a plan to the controller
// except as the result of an explicit stage save/delete by the user, and a
// stage save always carries the controller's full current stage list. The
// store is kept so a plan saved here earlier is not lost.
//
// The controller takes a plan only as one message (~1 KB per stage) and
// holds very little memory (it reports ~1 KB free), so it can carry about
// five stages. A longer plan lives here, in the bridge's flash, and the
// bridge writes the controller a window of it -- the stage running today
// and the next one -- moving the window on as the dates pass.
//
// Per controller (up to SB_MAX_DEVICES): one header sector and data
// sectors with three stage slots each. Nothing is held in RAM except the
// one stage being read or written.
// ============================================================================

#define PLAN_STORE_MAX_STAGES   27
#define PLAN_STORE_STAGE_MAX    1352   // bytes of JSON per stage

typedef struct {
    bool     active;        // the bridge drives this controller's plan
    bool     running;       // plan started (enabled on the controller)
    uint16_t count;         // stages stored
    int16_t  window;        // index of the first stage on the controller, -1 none
    char     name[32];
} plan_store_info_t;

// Reads the header for this controller; false when it has no long plan.
bool plan_store_info(const char *mac, plan_store_info_t *out);

// Stage i as text into out (NUL-terminated); returns its length, 0 if none.
size_t plan_store_get(const char *mac, int i, char *out, size_t n);

// Adds or replaces a stage, keeping the stages sorted by startDate.
// Replaces the stage with the same stageId when there is one. Returns the
// new index, or -1 with a message in err (full, overlap, invalid).
int plan_store_put(const char *mac, const char *stage, char *err, size_t errsz);

// Removes the stage with this stageId.
bool plan_store_delete(const char *mac, long stage_id);

// Deletes the whole long plan for this controller.
bool plan_store_clear(const char *mac);

// Marks the plan as running/stopped and as driven by the bridge.
bool plan_store_set_running(const char *mac, bool running);
bool plan_store_set_active(const char *mac, bool active);

// Clears the "active" flag of every stored plan (the stages stay). Returns
// how many were active. Used at boot while SB_LONG_PLAN is 0.
int plan_store_deactivate_all(void);

// The index of the stage running on date today (packed), the first stage
// after it when today falls in a gap, or -1 when the plan is over.
int plan_store_current(const char *mac, int today);

// Records which stage index the controller was last given.
bool plan_store_set_window(const char *mac, int first);

// Copies the whole long plan of src_mac into the controller's current
// plan text form ("{"stage":[...]}") -- only for small plans; used for
// importing a controller's own plan. Returns stages imported.
int plan_store_import(const char *mac, const char *plan_text, size_t len);
