// System power state for fancurved: sleep/wake notifications, plus the lid,
// display and user-presence signals the fan policy keys off.
#ifndef POWERSTATE_H
#define POWERSTATE_H

#include <CoreFoundation/CoreFoundation.h>

// A reading of "is anybody using this machine right now".
// Every *_known flag is 0 when the underlying property is missing or has an
// unexpected type; callers must read "unknown" as "in use", so an OS change can
// only ever cost us the optimisation, never the cooling.
typedef struct {
    int      lid_closed;         // AppleClamshellState
    int      lid_known;          // 0 on desktop Macs — they have no such key
    int      graphics_on;        // System Capabilities & kIOPMSystemCapabilityGraphics
    unsigned caps_raw;           // the whole bitfield, logged so the above is debuggable
    int      caps_known;
    int      user_active;        // IOPMUserIsActive
    int      user_active_known;
    double   idle_sec;           // HIDIdleTime in seconds, or -1 if unreadable
} PowerSnapshot;

int  PowerOpen(void);                  // cache the registry handles; 1 on success
void PowerClose(void);
void PowerSample(PowerSnapshot *out);  // four registry reads; fine every tick
void PowerWakeDescription(char *buf, size_t len);  // last wake type/reason, for logging

// Sleep/wake notifications. Callbacks run inline on `rl`'s thread.
// onWillSleep MUST return quickly: the sleep is acknowledged the moment it
// does, and nothing else runs until the machine wakes.
typedef void (*PowerEventFn)(void *ctx);

int  PowerNotifyStart(CFRunLoopRef rl, PowerEventFn onWillSleep,
                      PowerEventFn onHasPoweredOn, void *ctx);
void PowerNotifyStop(void);

#endif
