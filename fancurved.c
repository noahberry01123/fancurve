// fancurved — root daemon. Applies the config file to the fans every 2s, and
// hands them back to Apple firmware whenever nobody is using the machine.
// Modes: auto (hand back to firmware), manual (fixed %), curve (temp-driven).
//
// States: active (apply the config), idle-closed (lid shut and unattended —
// fans released), asleep (released before the system goes down).
#include "fanapi.h"
#include "powerstate.h"
#include "smc.h"
#include <dispatch/dispatch.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TICK_SECONDS 2.0

// Entering the released state is debounced so one odd sample cannot make the
// fans stutter. Leaving it never is — that is always the safe direction.
#define IDLE_ENTER_TICKS 3

// Branch A. A closed machine whose graphics capability is down (dark wake, or
// the moments after the lid shuts) is nobody's idea of "in use", so we release
// it regardless of how warm it is: firmware auto still cools it. We only take
// control back if it gets genuinely hot, which is the one case where our curve
// has anything to offer over the firmware's.
#define DARK_RELEASE_C   80.0   // release below this
#define DARK_REENGAGE_C  85.0   // take control back above this

// Branch B. Clamshell driving an external display is a fully awake machine, so
// idle time means what it says and we want real evidence of absence.
#define CLAMSHELL_IDLE_SECONDS 600.0
#define MANUAL_RELEASE_C        60.0   // "cool" in manual mode, where `low` is meaningless
#define TEMP_HYSTERESIS_C        5.0

typedef enum { ST_ACTIVE, ST_IDLE_CLOSED, ST_ASLEEP } State;
static const char *kStateName[] = { "active", "idle-closed", "asleep" };

static State              g_state      = ST_ACTIVE;
static int                g_fans       = 0;
static int                g_idle_ticks = 0;
static int                g_just_woke  = 0;
static CFRunLoopTimerRef  g_timer      = NULL;

static void log_line(const char *fmt, ...) {
    char ts[24];
    time_t now = time(NULL);
    struct tm tm;
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime_r(&now, &tm));
    printf("%s fancurved: ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

// Hand every fan back to firmware. Returns how many writes actually landed —
// a short count on the sleep path means the fix silently didn't happen, so it
// gets logged rather than assumed.
static int release_fans(int fans) {
    int ok = 0;
    for (int i = 0; i < fans; i++)
        if (FanSetMode(i, 0)) ok++;
    return ok;
}

// The original control body, unchanged. `t` is passed in so a tick only sweeps
// the 22 temperature sensors once.
static void apply_config(const FanConfig *c, double t, int fans) {
    if (strcmp(c->mode, "manual") == 0) {
        for (int i = 0; i < fans; i++) { FanSetMode(i, 1); FanSetPercent(i, c->percent); }
    } else if (strcmp(c->mode, "curve") == 0) {
        double frac = (c->high > c->low && t >= 0) ? (t - c->low) / (c->high - c->low) : 0;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        for (int i = 0; i < fans; i++) { FanSetMode(i, 1); FanSetPercent(i, frac * 100.0); }
    } else { // auto
        release_fans(fans);
    }
}

// Should the fans be handed back to firmware right now? `t` is MaxTemp()'s
// reading (-1 when nothing was readable) and `now_idle` says whether we are
// already released, so the temperature terms can be hysteretic.
static int idle_closed_now(const PowerSnapshot *p, const FanConfig *c,
                           double t, int now_idle) {
    // Lid open, or hardware that has no lid at all: always our problem.
    if (!p->lid_known || !p->lid_closed) return 0;
    if (t < 0) return 0;   // no temperature reading — stay in control

    if (p->caps_known && !p->graphics_on) {
        // Branch A — no display is up, so nobody is looking at this machine.
        // Idle time is deliberately not consulted here: HIDIdleTime counts
        // against a clock that does not advance during sleep, so seconds after
        // a Power Nap wake it still reads "just used" no matter how long the
        // machine was actually shut. That is exactly the case this branch is
        // for, and gating it on idle time would defeat the whole fix.
        return t < (now_idle ? DARK_REENGAGE_C : DARK_RELEASE_C);
    }

    // Branch B — clamshell driving an external display. Awake, so idle time is
    // meaningful; require both presence signals to agree nobody is there, and
    // the machine to be cool, before letting go.
    if (!(p->user_active_known && !p->user_active)) return 0;
    if (p->idle_sec < CLAMSHELL_IDLE_SECONDS) return 0;
    double cool = (strcmp(c->mode, "curve") == 0) ? c->low : MANUAL_RELEASE_C;
    return t < (now_idle ? cool + TEMP_HYSTERESIS_C : cool);
}

static void log_transition(State to, const PowerSnapshot *p, double t) {
    char temp[16], idle[16];
    if (t >= 0) snprintf(temp, sizeof temp, "%.1fC", t);
    else        snprintf(temp, sizeof temp, "?");
    if (p->idle_sec >= 0) snprintf(idle, sizeof idle, "%.0fs", p->idle_sec);
    else                  snprintf(idle, sizeof idle, "?");

    log_line("state %s -> %s  lid=%s gfx=%d caps=0x%x userActive=%s idle=%s temp=%s",
             kStateName[g_state], kStateName[to],
             p->lid_known ? (p->lid_closed ? "closed" : "open") : "n/a",
             p->graphics_on, p->caps_raw,
             p->user_active_known ? (p->user_active ? "1" : "0") : "?",
             idle, temp);
}

static void on_tick(CFRunLoopTimerRef timer, void *info) {
    (void)timer; (void)info;
    if (g_state == ST_ASLEEP) return;   // nothing to do until we are woken

    // Re-read the count every tick instead of caching it once at startup: it is
    // a single SMC read, and it self-heals if the first read failed or the
    // connection went stale across a wake.
    int fans = FanCount();
    if (fans <= 0) {
        FanClose();
        if (!FanOpen()) return;
        fans = FanCount();
        if (fans <= 0) return;
    }
    g_fans = fans;

    FanConfig c;
    ConfigRead(&c);
    PowerSnapshot p;
    PowerSample(&p);
    double t = MaxTemp();

    int want_idle = idle_closed_now(&p, &c, t, g_state == ST_IDLE_CLOSED);
    if (!want_idle) {
        g_idle_ticks = 0;
    } else if (g_state != ST_IDLE_CLOSED) {
        // A fresh wake has no prior state to oscillate against, so let it
        // release at once rather than running the fans through the debounce.
        if (!g_just_woke && ++g_idle_ticks < IDLE_ENTER_TICKS) want_idle = 0;
    }
    g_just_woke = 0;

    State want = want_idle ? ST_IDLE_CLOSED : ST_ACTIVE;
    if (want != g_state) {
        log_transition(want, &p, t);
        g_state = want;
    }

    // Re-asserted every tick, like the original auto branch, so nothing else
    // touching the SMC can leave the fans latched behind our back.
    if (g_state == ST_IDLE_CLOSED) release_fans(fans);
    else                           apply_config(&c, t, fans);
}

static void on_will_sleep(void *ctx) {
    (void)ctx;
    int fans = g_fans > 0 ? g_fans : FanCount();
    int ok   = release_fans(fans);
    log_line("state %s -> asleep (released %d/%d fan(s))", kStateName[g_state], ok, fans);
    g_state = ST_ASLEEP;
}

static void on_wake(void *ctx) {
    (void)ctx;
    char why[224];
    PowerWakeDescription(why, sizeof why);
    log_line("state %s -> active (woke, %s)", kStateName[g_state], why);

    g_state      = ST_ACTIVE;
    g_idle_ticks = 0;
    g_just_woke  = 1;
    // Evaluate immediately. Without this there is a 2s window in which a dark
    // wake would spin the fans back up before we notice the lid is still shut.
    if (g_timer) CFRunLoopTimerSetNextFireDate(g_timer, CFAbsoluteTimeGetCurrent());
}

static void install_signal_handlers(void) {
    // Dispatch signal sources do not suppress the default disposition, so
    // without these the process is killed before the handler ever runs.
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT,  SIG_IGN);

    static const int sigs[] = { SIGTERM, SIGINT };
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        dispatch_source_t s = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sigs[i], 0,
            dispatch_get_main_queue());
        if (!s) continue;
        dispatch_source_set_event_handler(s, ^{ CFRunLoopStop(CFRunLoopGetMain()); });
        dispatch_resume(s);   // never cancelled; lives as long as the process
    }
}

int main(void) {
    // launchd points stdout at a file, so it is fully buffered by default and
    // transition lines would sit in the buffer indefinitely.
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (!FanOpen()) { fprintf(stderr, "fancurved: cannot open AppleSMC\n"); return 1; }
    if (!PowerOpen())
        fprintf(stderr, "fancurved: no IOPMrootDomain; lid/idle detection disabled\n");

    g_fans = FanCount();
    log_line("started: %d fan(s), config=%s", g_fans, kConfigPath);

    g_timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(),
                                   TICK_SECONDS, 0, 0, on_tick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), g_timer, kCFRunLoopDefaultMode);

    if (!PowerNotifyStart(CFRunLoopGetMain(), on_will_sleep, on_wake, NULL))
        fprintf(stderr, "fancurved: sleep/wake notifications unavailable; "
                        "fans will NOT be released on sleep\n");

    install_signal_handlers();
    CFRunLoopRun();   // returns when a signal source calls CFRunLoopStop

    int fans = g_fans > 0 ? g_fans : FanCount();
    release_fans(fans);   // leave fans in auto
    PowerNotifyStop();
    PowerClose();
    FanClose();
    log_line("stopped: %d fan(s) returned to macOS control", fans);
    return 0;
}
