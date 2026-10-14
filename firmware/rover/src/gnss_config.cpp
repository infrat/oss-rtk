#include "gnss_config.h"
#include <ctype.h>
#include <stdarg.h>
#include "logger.h"

// Command reference: Quectel LC29H Series & LC79H (AL) GNSS Protocol
// Specification V1.4. Only commands the spec lists for the RTK variants
// (BA, CA, DA, EA) are used; the AA-only ones ($PQTMSRR, $PQTMCOLD,
// $PAIR072 elevation mask, ...) are left out on purpose.

const char *const GNSS_MSG_NAMES[GNSS_MSG_COUNT] = {"GGA", "RMC", "GSA", "GSV", "VTG", "GLL", "GST", "ZDA"};

void GnssChanges::clear()
{
    rcvrMode = -1;
    for (int i = 0; i < GNSS_CNST_COUNT; i++)
        constellation[i] = -1;
    fixIntervalMs = -1;
    for (int i = 0; i < GNSS_MSG_COUNT; i++)
        msgRate[i] = -1;
    sbas = -1;
    navMode = -1;
}

bool GnssChanges::empty() const
{
    if (rcvrMode >= 0 || constellation[0] >= 0 || fixIntervalMs >= 0 || sbas >= 0 || navMode >= 0)
        return false;
    for (int i = 0; i < GNSS_MSG_COUNT; i++)
        if (msgRate[i] >= 0)
            return false;
    return true;
}

const char *gnssOpId(GnssOp op)
{
    switch (op)
    {
    case GnssOp::Read:
        return "read";
    case GnssOp::Apply:
        return "apply";
    case GnssOp::Reboot:
        return "reboot";
    case GnssOp::HotStart:
        return "hot";
    case GnssOp::WarmStart:
        return "warm";
    case GnssOp::ColdStart:
        return "cold";
    case GnssOp::Restore:
        return "restore";
    default:
        return "none";
    }
}

const char *gnssJobStateId(GnssJobState state)
{
    switch (state)
    {
    case GnssJobState::Busy:
        return "busy";
    case GnssJobState::Done:
        return "done";
    case GnssJobState::Failed:
        return "failed";
    default:
        return "idle";
    }
}

static const char *pqtmError(int code)
{
    switch (code)
    {
    case 1:
        return "invalid parameters";
    case 2:
        return "execution failed";
    case 3:
        return "not supported by this module";
    default:
        return "rejected";
    }
}

static const char *pairError(int code)
{
    switch (code)
    {
    case 2:
        return "command failed";
    case 3:
        return "not supported by this module";
    case 4:
        return "parameter error";
    default:
        return "rejected";
    }
}

// NMEA checksum: XOR of everything between '$' and '*'
static uint8_t nmeaChecksum(const char *body, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum ^= (uint8_t)body[i];
    return sum;
}

void GnssConfig::begin(UARTHandler *uart)
{
    _uart = uart;
    _status.state = GnssJobState::Idle;
    _status.op = GnssOp::None;
    // Nothing is known until the module answers: 0 would read as a real value
    GnssModuleState &m = _status.module;
    m.valid = false;
    m.rcvrMode = -1;
    for (int i = 0; i < GNSS_CNST_COUNT; i++)
        m.constellation[i] = -1;
    m.fixIntervalMs = -1;
    for (int i = 0; i < GNSS_MSG_COUNT; i++)
        m.msgRate[i] = -1;
    m.sbas = -1;
    m.navMode = -1;
}

bool GnssConfig::request(GnssOp op, const GnssChanges *changes)
{
    bool accepted = false;
    portENTER_CRITICAL(&_lock);
    if (!_pending && !_running)
    {
        _requestOp = op;
        if (changes)
            _requestChanges = *changes;
        else
            _requestChanges.clear();
        // Visible as busy right away, the UI polls straight after the POST
        _status.state = GnssJobState::Busy;
        _status.op = op;
        _status.step = 0;
        _status.steps = 0;
        _status.error[0] = '\0';
        _pending = true;
        accepted = true;
    }
    portEXIT_CRITICAL(&_lock);
    return accepted;
}

void GnssConfig::snapshot(GnssStatus &out)
{
    portENTER_CRITICAL(&_lock);
    out = _status;
    portEXIT_CRITICAL(&_lock);
}

void GnssConfig::addCommand(Kind kind, Target target, uint8_t index, bool optional, const char *format, ...)
{
    if (_count >= MAX_COMMANDS)
        return;
    Command &c = _commands[_count++];
    va_list args;
    va_start(args, format);
    vsnprintf(c.text, sizeof(c.text), format, args);
    va_end(args);
    c.kind = kind;
    c.target = target;
    c.index = index;
    c.optional = optional;
    c.waitMs = 0;
}

void GnssConfig::addReads()
{
    // The version is the only mandatory answer: no reply here means no module
    addCommand(Kind::Pqtm, Target::Version, 0, false, "PQTMVERNO");
    addCommand(Kind::Pqtm, Target::RcvrMode, 0, true, "PQTMCFGRCVRMODE,R");
    addCommand(Kind::Pqtm, Target::Constellation, 0, true, "PQTMCFGCNST,R");
    addCommand(Kind::Pqtm, Target::FixRate, 0, true, "PQTMCFGFIXRATE,R");
    for (uint8_t i = 0; i < GNSS_MSG_COUNT; i++)
        addCommand(Kind::Pqtm, Target::MsgRate, i, true, "PQTMCFGMSGRATE,R,%s", GNSS_MSG_NAMES[i]);
    addCommand(Kind::PairGet, Target::Sbas, 0, true, "PAIR411");
    addCommand(Kind::PairGet, Target::NavMode, 0, true, "PAIR081");
}

void GnssConfig::buildJob(GnssOp op, const GnssChanges &ch)
{
    _count = 0;
    _jobNeedsReboot = false;

    switch (op)
    {
    case GnssOp::Read:
        addReads();
        break;

    case GnssOp::Apply:
    {
        bool pairChanged = false;
        if (ch.rcvrMode >= 0)
        {
            addCommand(Kind::Pqtm, Target::None, 0, false, "PQTMCFGRCVRMODE,W,%d", ch.rcvrMode);
            _jobNeedsReboot = true;
        }
        if (ch.constellation[0] >= 0)
        {
            const int8_t *c = ch.constellation;
            addCommand(Kind::Pqtm, Target::None, 0, false, "PQTMCFGCNST,W,%d,%d,%d,%d,%d,%d", c[0], c[1], c[2], c[3], c[4], c[5]);
            _jobNeedsReboot = true;
        }
        if (ch.fixIntervalMs >= 0)
        {
            addCommand(Kind::Pqtm, Target::None, 0, false, "PQTMCFGFIXRATE,W,%ld", (long)ch.fixIntervalMs);
            _jobNeedsReboot = true;
        }
        for (uint8_t i = 0; i < GNSS_MSG_COUNT; i++)
        {
            if (ch.msgRate[i] >= 0)
                addCommand(Kind::Pqtm, Target::None, 0, false, "PQTMCFGMSGRATE,W,%s,%d", GNSS_MSG_NAMES[i], ch.msgRate[i]);
        }
        if (ch.sbas >= 0)
        {
            addCommand(Kind::PairSet, Target::None, 0, false, "PAIR410,%d", ch.sbas);
            pairChanged = true;
        }
        if (ch.navMode >= 0)
        {
            addCommand(Kind::PairSet, Target::None, 0, false, "PAIR080,%d", ch.navMode);
            pairChanged = true;
        }

        // Into the module's NVM - nothing is kept on the ESP32
        addCommand(Kind::Pqtm, Target::None, 0, false, "PQTMSAVEPAR");

        if (pairChanged)
        {
            // $PAIR settings are saved by $PAIR513. Above 1 Hz the spec wants
            // the GNSS engine powered off around it.
            int32_t interval = _status.module.fixIntervalMs;
            if (interval > 0 && interval < 1000)
            {
                addCommand(Kind::PairSet, Target::None, 0, false, "PAIR382,1");
                addCommand(Kind::PairSet, Target::None, 0, false, "PAIR003");
                addCommand(Kind::PairSet, Target::None, 0, false, "PAIR513");
                addCommand(Kind::Blind, Target::None, 0, false, "");
                _commands[_count - 1].waitMs = GNSS_POWER_CYCLE_MS;
                addCommand(Kind::PairSet, Target::None, 0, false, "PAIR002");
            }
            else
            {
                addCommand(Kind::PairSet, Target::None, 0, false, "PAIR513");
            }
        }

        // Show what the module now reports, not what was requested
        addReads();
        break;
    }

    case GnssOp::Reboot:
        // Not in the protocol spec; Quectel support documents it on their
        // forum as the LC29H reboot command. There is no reply.
        addCommand(Kind::Blind, Target::None, 0, false, "PAIR023");
        _commands[_count - 1].waitMs = GNSS_REBOOT_WAIT_MS;
        addReads();
        break;

    case GnssOp::HotStart:
        addCommand(Kind::PairSet, Target::None, 0, false, "PAIR004");
        break;

    case GnssOp::WarmStart:
        addCommand(Kind::PairSet, Target::None, 0, false, "PAIR005");
        break;

    case GnssOp::ColdStart:
        addCommand(Kind::PairSet, Target::None, 0, false, "PAIR006");
        break;

    case GnssOp::Restore:
        // Factory values for everything set by $PQTM, $PAIR050 and $PAIR062.
        // Applies after the next module reboot.
        addCommand(Kind::Pqtm, Target::None, 0, false, "PQTMRESTOREPAR");
        _jobNeedsReboot = true;
        break;

    default:
        break;
    }
}

void GnssConfig::loop()
{
    if (!_uart)
        return;

    if (_pending && !_running)
    {
        GnssOp op;
        GnssChanges changes;
        portENTER_CRITICAL(&_lock);
        op = _requestOp;
        changes = _requestChanges;
        portEXIT_CRITICAL(&_lock);

        buildJob(op, changes);
        if (_count == 0)
        {
            _running = true; // finish() clears it
            _pending = false;
            finish(false, "Nothing to do");
            return;
        }

        // A client's RTCM stream is cut wherever the bridge paused. The module
        // then waits for the rest of that frame and swallows the commands as
        // its payload, which looks like a module that does not respond. Blank
        // lines complete the frame: its CRC fails and the parser starts over.
        uint8_t pad[64];
        for (size_t i = 0; i < sizeof(pad); i++)
            pad[i] = (i & 1) ? '\n' : '\r';
        for (size_t sent = 0; sent < GNSS_FLUSH_BYTES; sent += sizeof(pad))
            _uart->writeData(pad, sizeof(pad));

        // Stale NMEA in the buffer is of no use to a command/response exchange
        uint8_t scratch[128];
        int pending;
        while ((pending = _uart->available()) > 0)
            _uart->readData(scratch, min((size_t)pending, sizeof(scratch)));
        _lineLen = 0;

        logPrintln("[GNSS] %s: %u command(s)", gnssOpId(op), _count);
        _running = true; // before clearing _pending: active() must never blink false
        _pending = false;
        _current = 0;
        _missedCount = 0;
        startCommand();
        return;
    }

    if (!_running)
        return;

    // Assemble lines. The module keeps streaming NMEA, the answers are
    // somewhere in between.
    uint8_t chunk[128];
    int available;
    while (_running && (available = _uart->available()) > 0)
    {
        int n = _uart->readData(chunk, min((size_t)available, sizeof(chunk)));
        if (n > 0)
            _rxBytes += n;
        for (int i = 0; i < n && _running; i++)
        {
            char ch = (char)chunk[i];
            if (ch == '$')
            {
                _line[0] = '$';
                _lineLen = 1;
            }
            else if (ch == '\n' || ch == '\r')
            {
                if (_lineLen > 1 && _lineLen < sizeof(_line))
                {
                    _line[_lineLen] = '\0';
                    handleLine(_line);
                }
                _lineLen = 0;
            }
            else if (_lineLen > 0 && _lineLen < sizeof(_line) - 1)
            {
                _line[_lineLen++] = ch;
            }
            else if (_lineLen > 0)
            {
                _lineLen = sizeof(_line); // too long, drop until the next line
            }
        }
    }

    if (!_running)
        return;

    const Command &c = _commands[_current];
    if ((int32_t)(millis() - _deadlineMs) < 0)
        return;

    if (c.kind == Kind::Blind)
    {
        commandDone(true, nullptr, 0, nullptr);
    }
    else if (_attempt < GNSS_CMD_RETRIES)
    {
        logSilence();
        _attempt++;
        sendCurrent();
    }
    else
    {
        logSilence();
        commandDone(false, nullptr, 0, "no response");
    }
}

void GnssConfig::logSilence()
{
    logPrintln("[GNSS] no answer to %s: %u bytes, %u sentences (%u more with a bad checksum) came in meanwhile",
               _commands[_current].text, (unsigned)_rxBytes, (unsigned)_rxLines, (unsigned)_rxBad);
}

void GnssConfig::startCommand()
{
    _attempt = 0;
    portENTER_CRITICAL(&_lock);
    _status.step = _current + 1;
    _status.steps = _count;
    portEXIT_CRITICAL(&_lock);
    sendCurrent();
}

void GnssConfig::sendCurrent()
{
    const Command &c = _commands[_current];
    _deadlineMs = millis() + (c.kind == Kind::Blind ? c.waitMs : GNSS_CMD_TIMEOUT_MS);
    _rxBytes = 0;
    _rxLines = 0;
    _rxBad = 0;

    if (c.text[0] == '\0')
        return; // pure wait step

    char frame[sizeof(c.text) + 8];
    snprintf(frame, sizeof(frame), "$%s*%02X\r\n", c.text, nmeaChecksum(c.text, strlen(c.text)));
    _uart->writeString(frame);
    logPrintln("[GNSS] > %s%s", c.text, _attempt ? " (retry)" : "");
}

void GnssConfig::handleLine(char *line)
{
    // "$BODY*HH": verify and strip the checksum
    char *star = strrchr(line, '*');
    if (!star || strlen(star) < 3)
    {
        _rxBad++;
        return;
    }
    char hex[3] = {star[1], star[2], '\0'};
    if (nmeaChecksum(line + 1, star - line - 1) != (uint8_t)strtoul(hex, nullptr, 16))
    {
        _rxBad++;
        return;
    }
    *star = '\0';
    _rxLines++;

    const Command &c = _commands[_current];
    if (c.kind == Kind::Blind)
        return;

    // Only proprietary answers matter; skip the regular NMEA stream cheaply
    if (strncmp(line, "$PQTM", 5) != 0 && strncmp(line, "$PAIR", 5) != 0)
        return;

    // Command name = text up to the first comma
    char name[24];
    size_t nameLen = strcspn(c.text, ",");
    if (nameLen >= sizeof(name))
        nameLen = sizeof(name) - 1;
    memcpy(name, c.text, nameLen);
    name[nameLen] = '\0';

    // Split "$NAME,a,b,c" in place (empty fields are kept); keep the whole
    // sentence for the log first
    char raw[sizeof(_line)];
    strlcpy(raw, line + 1, sizeof(raw));
    const char *fields[12];
    int count = 0;
    char *p = line + 1;
    fields[count++] = p;
    while ((p = strchr(p, ',')) != nullptr && count < 12)
    {
        *p++ = '\0';
        fields[count++] = p;
    }

    if (c.kind == Kind::Pqtm)
    {
        if (strcmp(fields[0], name) != 0)
        {
            logPrintln("[GNSS] < %s (not the answer to %s, ignored)", raw, c.text);
            return;
        }
        logPrintln("[GNSS] < %s", raw);
        if (count >= 2 && strcmp(fields[1], "ERROR") == 0)
        {
            commandDone(false, nullptr, 0, pqtmError(count >= 3 ? atoi(fields[2]) : 0));
            return;
        }
        // $PQTMVERNO answers with the data directly, everything else with OK
        if (c.target != Target::Version && (count < 2 || strcmp(fields[1], "OK") != 0))
        {
            commandDone(false, nullptr, 0, "unexpected answer");
            return;
        }
        commandDone(true, fields, count, nullptr);
        return;
    }

    // $PAIR: "PAIR410" → id "410"
    const char *id = name + 4;

    if (strcmp(fields[0], "PAIR001") == 0)
    {
        if (count < 3 || strcmp(fields[1], id) != 0)
        {
            logPrintln("[GNSS] < %s (not the answer to %s, ignored)", raw, c.text);
            return;
        }
        logPrintln("[GNSS] < %s", raw);
        int result = atoi(fields[2]);
        if (result == 1 || result == 5)
        {
            // 1 = still processing: keep waiting. 5 = busy: the timeout resends.
            if (result == 1)
                _deadlineMs = millis() + GNSS_CMD_TIMEOUT_MS;
            return;
        }
        if (result != 0)
        {
            commandDone(false, nullptr, 0, pairError(result));
            return;
        }
        if (c.kind == Kind::PairSet)
        {
            commandDone(true, fields, count, nullptr);
            return;
        }
        // PairGet: the value follows in its own sentence
        _deadlineMs = millis() + GNSS_CMD_TIMEOUT_MS;
        return;
    }

    if (c.kind == Kind::PairGet && strncmp(fields[0], "PAIR", 4) == 0 && strcmp(fields[0] + 4, id) == 0)
    {
        logPrintln("[GNSS] < %s", raw);
        commandDone(true, fields, count, nullptr);
        return;
    }

    logPrintln("[GNSS] < %s (not the answer to %s, ignored)", raw, c.text);
}

void GnssConfig::markUnsupported(const Command &cmd)
{
    portENTER_CRITICAL(&_lock);
    GnssModuleState &m = _status.module;
    switch (cmd.target)
    {
    case Target::RcvrMode:
        m.rcvrMode = -1;
        break;
    case Target::Constellation:
        for (int i = 0; i < GNSS_CNST_COUNT; i++)
            m.constellation[i] = -1;
        break;
    case Target::FixRate:
        m.fixIntervalMs = -1;
        break;
    case Target::MsgRate:
        m.msgRate[cmd.index] = -1;
        break;
    case Target::Sbas:
        m.sbas = -1;
        break;
    case Target::NavMode:
        m.navMode = -1;
        break;
    default:
        break;
    }
    portEXIT_CRITICAL(&_lock);
}

void GnssConfig::store(const Command &cmd, const char *const *f, int n)
{
    // Field layout: PQTM answers "NAME,OK,<values>", PAIR gets "PAIRnnn,<values>"
    portENTER_CRITICAL(&_lock);
    GnssModuleState &m = _status.module;
    switch (cmd.target)
    {
    case Target::Version:
        if (n >= 2)
        {
            strlcpy(m.version, f[1], sizeof(m.version));
            // "LC29HDANR11A03S" → "DA"
            m.variant[0] = '\0';
            if (strncmp(m.version, "LC29H", 5) == 0 && isupper((unsigned char)m.version[5]) && isupper((unsigned char)m.version[6]))
            {
                m.variant[0] = m.version[5];
                m.variant[1] = m.version[6];
                m.variant[2] = '\0';
            }
        }
        break;
    case Target::RcvrMode:
        m.rcvrMode = n >= 3 ? (int8_t)atoi(f[2]) : -1;
        break;
    case Target::Constellation:
        for (int i = 0; i < GNSS_CNST_COUNT; i++)
            m.constellation[i] = (2 + i) < n ? (int8_t)atoi(f[2 + i]) : 0;
        break;
    case Target::FixRate:
        m.fixIntervalMs = n >= 3 ? atol(f[2]) : -1;
        break;
    case Target::MsgRate:
        // "PQTMCFGMSGRATE,OK,GGA,1," - make sure it is the sentence we asked for
        m.msgRate[cmd.index] = (n >= 4 && strcmp(f[2], GNSS_MSG_NAMES[cmd.index]) == 0) ? (int8_t)atoi(f[3]) : -1;
        break;
    case Target::Sbas:
        m.sbas = n >= 2 ? (int8_t)atoi(f[1]) : -1;
        break;
    case Target::NavMode:
        m.navMode = n >= 2 ? (int8_t)atoi(f[1]) : -1;
        break;
    default:
        break;
    }
    portEXIT_CRITICAL(&_lock);
}

void GnssConfig::commandDone(bool ok, const char *const *fields, int count, const char *error)
{
    const Command &c = _commands[_current];
    if (ok)
    {
        store(c, fields, count);
    }
    else if (c.optional && strcmp(error, "no response") == 0)
    {
        // Silence is not a "no": the field keeps its last known value and the
        // job is reported as incomplete
        if (_missedCount++ == 0)
            strlcpy(_missed, c.text, sizeof(_missed));
        logPrintln("[GNSS] %s: no response - keeping the last known value", c.text);
    }
    else if (c.optional)
    {
        markUnsupported(c);
        logPrintln("[GNSS] %s: %s - treated as unsupported", c.text, error);
    }
    else
    {
        char message[sizeof(_status.error)];
        if (c.target == Target::Version && strcmp(error, "no response") == 0)
            snprintf(message, sizeof(message), "GNSS module not responding (check wiring and %d baud)", GPS_BAUD_RATE);
        else
            snprintf(message, sizeof(message), "%s: %s", c.text, error);
        finish(false, message);
        return;
    }

    _current++;
    if (_current >= _count)
        finish(true, nullptr);
    else
        startCommand();
}

void GnssConfig::finish(bool ok, const char *error)
{
    bool hasReads = false;
    for (uint8_t i = 0; i < _count; i++)
        if (_commands[i].target == Target::Version)
            hasReads = true;

    // Everything mandatory went through but some probes got no answer: the
    // results are kept and the job still counts as failed, so the UI says so
    char incomplete[sizeof(_status.error)];
    if (ok && _missedCount > 0)
    {
        if (_missedCount == 1)
            snprintf(incomplete, sizeof(incomplete), "No answer from the module to %s. Read again.", _missed);
        else
            snprintf(incomplete, sizeof(incomplete), "No answer from the module to %s and %u more. Read again.",
                     _missed, (unsigned)(_missedCount - 1));
        error = incomplete;
    }
    bool complete = ok && _missedCount == 0;

    portENTER_CRITICAL(&_lock);
    _status.state = complete ? GnssJobState::Done : GnssJobState::Failed;
    strlcpy(_status.error, error ? error : "", sizeof(_status.error));
    if (ok)
    {
        if (hasReads)
            _status.module.valid = true;
        if (_status.op == GnssOp::Reboot)
            _status.needsReboot = false;
        if (_jobNeedsReboot)
            _status.needsReboot = true;
    }
    portEXIT_CRITICAL(&_lock);

    _running = false;
    if (complete)
        logPrintln("[GNSS] %s done", gnssOpId(_status.op));
    else
        logPrintln("[GNSS] %s failed: %s", gnssOpId(_status.op), error);
}
