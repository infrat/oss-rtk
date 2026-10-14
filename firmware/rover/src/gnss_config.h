#ifndef GNSS_CONFIG_H
#define GNSS_CONFIG_H

#include <Arduino.h>
#include "config.h"
#include "uart_handler.h"

// Standard NMEA sentences whose output rate the web UI exposes, in UI order.
// Configured through $PQTMCFGMSGRATE; the ones a variant lacks are reported
// as unsupported (-1) after the read probe.
#define GNSS_MSG_COUNT 8
extern const char *const GNSS_MSG_NAMES[GNSS_MSG_COUNT];

#define GNSS_CNST_COUNT 6 // GPS, GLONASS, Galileo, BDS, QZSS, NavIC ($PQTMCFGCNST order)

/**
 * @brief What the LC29H last reported. -1 = the module rejected the query
 *        (not supported on this variant) or has never answered it. A query
 *        that merely times out keeps the previous value.
 */
struct GnssModuleState
{
    bool valid;     // a read finished since boot
    char version[48];
    char variant[3]; // "DA", "BA", ... parsed from the version string
    int8_t rcvrMode; // 1 = rover, 2 = base
    int8_t constellation[GNSS_CNST_COUNT];
    int32_t fixIntervalMs;
    int8_t msgRate[GNSS_MSG_COUNT]; // 0 = off, N = every N fixes
    int8_t sbas;                    // 0/1
    int8_t navMode;                 // $PAIR080 values
};

/**
 * @brief Settings to write. Negative = leave unchanged.
 */
struct GnssChanges
{
    int8_t rcvrMode;
    int8_t constellation[GNSS_CNST_COUNT]; // [0] < 0 = unchanged
    int32_t fixIntervalMs;
    int8_t msgRate[GNSS_MSG_COUNT];
    int8_t sbas;
    int8_t navMode;

    void clear();
    bool empty() const;
};

enum class GnssOp : uint8_t
{
    None,
    Read,
    Apply,
    Reboot,
    HotStart,
    WarmStart,
    ColdStart,
    Restore
};

enum class GnssJobState : uint8_t
{
    Idle,
    Busy,
    Done,
    Failed
};

/**
 * @brief Copy of the job state for the web UI (plain data, taken under a lock).
 */
struct GnssStatus
{
    GnssJobState state;
    GnssOp op;
    uint8_t step;
    uint8_t steps;
    bool needsReboot; // saved settings that only apply after a module reboot
    char error[96];
    GnssModuleState module;
};

const char *gnssOpId(GnssOp op);
const char *gnssJobStateId(GnssJobState state);

/**
 * @brief Talks $PQTM / $PAIR to the LC29H over the GNSS UART.
 *
 * Requests come from the web server task; the commands run from the main
 * loop as a non-blocking state machine (one command in flight, response or
 * timeout, next). While a job is active it owns the UART: the main loop must
 * not run the bridge or drain the UART (see active()).
 *
 * Nothing is stored on the ESP32: every change is saved into the module's
 * own NVM with $PQTMSAVEPAR, and the UI always shows what the module reports.
 */
class GnssConfig
{
public:
    void begin(UARTHandler *uart);

    // Web server task. Returns false when a job is already queued or running.
    bool request(GnssOp op, const GnssChanges *changes = nullptr);

    // Any task
    void snapshot(GnssStatus &out);

    // True while a job is queued or running: the UART belongs to this class
    bool active() const { return _pending || _running; }

    // Main loop
    void loop();

private:
    enum class Kind : uint8_t
    {
        Pqtm,    // answers $<NAME>,OK[,...] or $<NAME>,ERROR,<code>
        PairSet, // answers $PAIR001,<id>,<result>
        PairGet, // $PAIR001 ack, then $PAIR<id>,<value>
        Blind    // no answer, wait a fixed time ($PAIR023 reboot)
    };

    enum class Target : uint8_t
    {
        None,
        Version,
        RcvrMode,
        Constellation,
        FixRate,
        MsgRate, // index = message
        Sbas,
        NavMode
    };

    struct Command
    {
        char text[40]; // without '$' and checksum
        Kind kind;
        Target target;
        uint8_t index;
        bool optional;   // a probe: a rejection marks the field unsupported, the job goes on
        uint16_t waitMs; // Blind only
    };

    static const uint8_t MAX_COMMANDS = 40;

    void buildJob(GnssOp op, const GnssChanges &changes);
    void addCommand(Kind kind, Target target, uint8_t index, bool optional, const char *format, ...);
    void addReads();
    void startCommand();
    void sendCurrent();
    void handleLine(char *line);
    void commandDone(bool ok, const char *const *fields, int count, const char *error);
    void finish(bool ok, const char *error);
    void markUnsupported(const Command &cmd);
    void logSilence();
    void store(const Command &cmd, const char *const *fields, int count);

    UARTHandler *_uart = nullptr;
    portMUX_TYPE _lock = portMUX_INITIALIZER_UNLOCKED;

    // Request mailbox (web task → main loop)
    volatile bool _pending = false;
    GnssOp _requestOp = GnssOp::None;
    GnssChanges _requestChanges;

    // Job (main loop only, status fields read under _lock)
    bool _running = false;
    Command _commands[MAX_COMMANDS];
    uint8_t _count = 0;
    uint8_t _current = 0;
    uint8_t _attempt = 0;
    uint32_t _deadlineMs = 0;
    bool _jobNeedsReboot = false; // set by buildJob, applied to the status on success

    char _line[128];
    size_t _lineLen = 0;

    // Probes that timed out in this job (their fields keep the old value)
    char _missed[sizeof(Command::text)];
    uint8_t _missedCount = 0;

    // What arrived since the current command was sent, for the timeout log:
    // tells a silent module from an answer lost on the way in
    uint32_t _rxBytes = 0;
    uint16_t _rxLines = 0;
    uint16_t _rxBad = 0;

    GnssStatus _status = {};
};

#endif // GNSS_CONFIG_H
