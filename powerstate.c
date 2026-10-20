// fancurved runs as root under launchd, outside any GUI login session, so
// CoreGraphics (CGDisplayIsAsleep) and NSWorkspace are unavailable to it.
// Everything here is an IORegistry read or an IOKit notification, both of which
// work fine from that context.
#include "powerstate.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/hidsystem/IOHIDParameter.h>   // kIOHIDIdleTimeKey
#include <IOKit/hidsystem/IOHIDShared.h>      // kIOHIDSystemClass
#include <string.h>

// AppleClamshellState (IOPM.h) and HIDIdleTime (IOHIDParameter.h) are documented
// constants. These three are not in any public header — they are read off
// IOPMrootDomain at runtime. The documented alternative for capabilities,
// IOPMConnectionGetSystemCapabilities, is SPI with no header at all, so this is
// the lesser evil; every read below falls back to "in use" if a key disappears.
#define kSystemCapabilitiesKey  "System Capabilities"
#define kUserIsActiveKey        "IOPMUserIsActive"
#define kWakeTypeKey            "Wake Type"
#define kWakeReasonKey          "Wake Reason"

static io_service_t g_root = IO_OBJECT_NULL;   // IOPMrootDomain
static io_service_t g_hid  = IO_OBJECT_NULL;   // IOHIDSystem

static io_connect_t          g_pmPort     = MACH_PORT_NULL;
static io_object_t           g_pmNotifier = IO_OBJECT_NULL;
static IONotificationPortRef g_pmNote     = NULL;
static CFRunLoopRef          g_pmLoop     = NULL;
static PowerEventFn          g_onWillSleep = NULL;
static PowerEventFn          g_onWake      = NULL;
static void                 *g_ctx         = NULL;

// --- registry reads ---------------------------------------------------------
// Each returns 1 only when the key exists AND has the type we expect, so a
// future OS renaming or retyping a key degrades instead of returning garbage.

static int read_bool(io_service_t svc, CFStringRef key, int *out) {
    if (!svc) return 0;
    CFTypeRef v = IORegistryEntryCreateCFProperty(svc, key, kCFAllocatorDefault, 0);
    if (!v) return 0;
    int ok = 0;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
        *out = CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
        ok = 1;
    }
    CFRelease(v);
    return ok;
}

static int read_i64(io_service_t svc, CFStringRef key, int64_t *out) {
    if (!svc) return 0;
    CFTypeRef v = IORegistryEntryCreateCFProperty(svc, key, kCFAllocatorDefault, 0);
    if (!v) return 0;
    int ok = 0;
    if (CFGetTypeID(v) == CFNumberGetTypeID())
        ok = CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, out);
    CFRelease(v);
    return ok;
}

static int read_str(io_service_t svc, CFStringRef key, char *buf, size_t len) {
    if (!svc || len == 0) return 0;
    CFTypeRef v = IORegistryEntryCreateCFProperty(svc, key, kCFAllocatorDefault, 0);
    if (!v) return 0;
    int ok = 0;
    if (CFGetTypeID(v) == CFStringGetTypeID())
        ok = CFStringGetCString((CFStringRef)v, buf, (CFIndex)len, kCFStringEncodingUTF8);
    CFRelease(v);
    return ok;
}

int PowerOpen(void) {
    g_root = IOServiceGetMatchingService(kIOMainPortDefault,
                                         IOServiceMatching("IOPMrootDomain"));
    g_hid  = IOServiceGetMatchingService(kIOMainPortDefault,
                                         IOServiceMatching(kIOHIDSystemClass));
    // Both entries are stable for the life of the process — neither detaches,
    // including across sleep — so they are cached rather than re-matched.
    return g_root != IO_OBJECT_NULL;
}

void PowerClose(void) {
    if (g_root) { IOObjectRelease(g_root); g_root = IO_OBJECT_NULL; }
    if (g_hid)  { IOObjectRelease(g_hid);  g_hid  = IO_OBJECT_NULL; }
}

void PowerSample(PowerSnapshot *s) {
    memset(s, 0, sizeof *s);
    // Defaults are the fan-controlling ones. read_* only writes on success, so
    // anything we cannot read leaves the machine looking awake and in use.
    s->graphics_on = 1;
    s->user_active = 1;
    s->idle_sec    = -1;

    s->lid_known         = read_bool(g_root, CFSTR(kAppleClamshellStateKey), &s->lid_closed);
    s->user_active_known = read_bool(g_root, CFSTR(kUserIsActiveKey), &s->user_active);

    int64_t caps = 0;
    if (read_i64(g_root, CFSTR(kSystemCapabilitiesKey), &caps)) {
        s->caps_known  = 1;
        s->caps_raw    = (unsigned)caps;
        s->graphics_on = (caps & kIOPMSystemCapabilityGraphics) != 0;
    }

    int64_t ns = 0;
    if (read_i64(g_hid, CFSTR(kIOHIDIdleTimeKey), &ns) && ns >= 0)
        s->idle_sec = (double)ns / 1e9;
}

void PowerWakeDescription(char *buf, size_t len) {
    char type[96] = "?", reason[96] = "?";
    read_str(g_root, CFSTR(kWakeTypeKey),   type,   sizeof type);
    read_str(g_root, CFSTR(kWakeReasonKey), reason, sizeof reason);
    // Latched snapshots of the *last* wake — good for the log, never for a
    // decision: they go stale the moment a dark wake is promoted to a full one.
    snprintf(buf, len, "type=\"%s\" reason=\"%s\"", type, reason);
}

// --- sleep/wake notifications -----------------------------------------------

static void pm_callback(void *refcon, io_service_t svc, natural_t type, void *arg) {
    (void)refcon; (void)svc;
    switch (type) {

    case kIOMessageCanSystemSleep:
        // The idle-sleep poll. Nothing is released here: this attempt can still
        // be withdrawn (kIOMessageSystemWillNotSleep). We never veto, but we do
        // have to answer — staying silent stalls every idle sleep for 30s.
        IOAllowPowerChange(g_pmPort, (intptr_t)arg);
        break;

    case kIOMessageSystemWillSleep:
        // Non-abortable, and delivered before any hardware is powered off, so
        // SMC writes still land. The release must happen synchronously right
        // here — never deferred to the run loop, which is not serviced again
        // until the machine wakes. Budget is 30s; a few SMC writes take ms.
        if (g_onWillSleep) g_onWillSleep(g_ctx);
        IOAllowPowerChange(g_pmPort, (intptr_t)arg);
        break;

    case kIOMessageSystemHasPoweredOn:
        if (g_onWake) g_onWake(g_ctx);
        break;

    // kIOMessageSystemWillPowerOn and kIOMessageSystemWillNotSleep deliberately
    // fall through: IOAllowPowerChange is only ever for the two cases above.
    default:
        break;
    }
}

int PowerNotifyStart(CFRunLoopRef rl, PowerEventFn onWillSleep,
                     PowerEventFn onHasPoweredOn, void *ctx) {
    g_onWillSleep = onWillSleep;
    g_onWake      = onHasPoweredOn;
    g_ctx         = ctx;

    g_pmPort = IORegisterForSystemPower(NULL, &g_pmNote, pm_callback, &g_pmNotifier);
    if (g_pmPort == MACH_PORT_NULL) return 0;

    g_pmLoop = rl;
    CFRunLoopAddSource(rl, IONotificationPortGetRunLoopSource(g_pmNote),
                       kCFRunLoopDefaultMode);
    return 1;
}

void PowerNotifyStop(void) {
    if (g_pmPort == MACH_PORT_NULL) return;
    if (g_pmLoop)
        CFRunLoopRemoveSource(g_pmLoop, IONotificationPortGetRunLoopSource(g_pmNote),
                              kCFRunLoopDefaultMode);
    // Teardown order is prescribed by IOPMLib.h: deregister, close the connect,
    // then destroy the port.
    IODeregisterForSystemPower(&g_pmNotifier);
    IOServiceClose(g_pmPort);
    IONotificationPortDestroy(g_pmNote);
    g_pmPort = MACH_PORT_NULL;
    g_pmNote = NULL;
    g_pmLoop = NULL;
}
