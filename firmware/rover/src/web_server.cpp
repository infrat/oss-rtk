#include "web_server.h"
#include <AsyncJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include "config.h"
#include "logger.h"
#include "system_control.h"

void RoverWebServer::begin(RoverSettings *settings, SettingsStore *store, WifiManager *wifi, ITransport *const *transport, GnssConfig *gnss)
{
    _settings = settings;
    _store = store;
    _wifi = wifi;
    _transport = transport;
    _gnss = gnss;

    // true = format on first use, so a fresh partition never blocks the boot
    if (!LittleFS.begin(true))
    {
        logPrintln("[Web] LittleFS mount failed - UI files unavailable");
    }

    _server = new AsyncWebServer(HTTP_PORT);

    _server->on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request)
                { handleStatus(request); });

    _server->on("/api/config", HTTP_GET, [this](AsyncWebServerRequest *request)
                {
                    _wifi->keepAlive();
                    handleGetConfig(request); });

    AsyncCallbackJsonWebHandler *saveHandler = new AsyncCallbackJsonWebHandler(
        "/api/config", [this](AsyncWebServerRequest *request, JsonVariant &json)
        {
            _wifi->keepAlive();
            handleSaveConfig(request, json); });
    saveHandler->setMethod(HTTP_POST);
    saveHandler->setMaxContentLength(1024);
    _server->addHandler(saveHandler);

    _server->on("/api/restart", HTTP_POST, [this](AsyncWebServerRequest *request)
                {
                    _wifi->keepAlive();
                    request->send(200, "application/json", "{\"success\":true,\"message\":\"Device restarting...\"}");
                    scheduleRestart(1000); });

    _server->on("/api/wifi/off", HTTP_POST, [this](AsyncWebServerRequest *request)
                {
                    if (_wifi->alwaysOn())
                    {
                        sendError(request, 400, "WiFi stays on with the WiFi (TCP) link");
                        return;
                    }
                    request->send(200, "application/json", "{\"success\":true,\"message\":\"WiFi off, starting Bluetooth\"}");
                    _wifi->finishSoon(500); });

    _server->on("/api/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest *request)
                {
                    _wifi->keepAlive();
                    handleScan(request); });

    _server->on("/api/gnss", HTTP_GET, [this](AsyncWebServerRequest *request)
                {
                    _wifi->keepAlive();
                    handleGnssStatus(request); });

    AsyncCallbackJsonWebHandler *gnssHandler = new AsyncCallbackJsonWebHandler(
        "/api/gnss", [this](AsyncWebServerRequest *request, JsonVariant &json)
        {
            _wifi->keepAlive();
            handleGnssRequest(request, json); });
    gnssHandler->setMethod(HTTP_POST);
    gnssHandler->setMaxContentLength(512);
    _server->addHandler(gnssHandler);

    _server->on("/", HTTP_GET, [](AsyncWebServerRequest *request)
                {
                    if (LittleFS.exists("/index.html"))
                    {
                        request->send(LittleFS, "/index.html", "text/html");
                    }
                    else
                    {
                        request->send(200, "text/html",
                                      "<html><body><h1>OSS RTK Rover</h1>"
                                      "<p>Web UI files not found. Upload the filesystem image "
                                      "(pio run -t uploadfs).</p></body></html>");
                    } });

    _server->serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    _server->onNotFound([](AsyncWebServerRequest *request)
                        { request->send(404, "text/plain", "Not found"); });

    _server->begin();
    logPrintln("[Web] UI on port %d", HTTP_PORT);
}

void RoverWebServer::end()
{
    if (_server)
    {
        _server->end();
    }
}

void RoverWebServer::sendError(AsyncWebServerRequest *request, int code, const char *message)
{
    StaticJsonDocument<192> doc;
    doc["success"] = false;
    doc["message"] = message;
    String out;
    serializeJson(doc, out);
    request->send(code, "application/json", out);
}

void RoverWebServer::handleStatus(AsyncWebServerRequest *request)
{
    // Only the page's explicit heartbeat extends the setup window. It stops when
    // the tab is hidden or unused, so a forgotten tab cannot keep Bluetooth off.
    if (request->hasParam("ka"))
    {
        _wifi->keepAlive();
    }

    StaticJsonDocument<768> doc;
    doc["version"] = FIRMWARE_VERSION;
    doc["uptime"] = millis() / 1000;
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["maxAlloc"] = ESP.getMaxAllocHeap(); // largest free block: what a response buffer can get
    doc["minFreeHeap"] = ESP.getMinFreeHeap();

    ITransport *t = _transport ? *_transport : nullptr;
    JsonObject tr = doc.createNestedObject("transport");
    tr["mode"] = transportModeId(_settings->transport);
    tr["name"] = t ? t->name() : "-";
    tr["active"] = t != nullptr; // false during the WiFi-only setup window
    tr["connected"] = t ? t->isConnected() : false;
    tr["clients"] = t ? t->getConnectedCount() : 0;

    JsonObject w = doc.createNestedObject("wifi");
    w["mode"] = wifiModeId(_wifi->mode());
    w["enabled"] = _wifi->enabled();
    w["remainingSec"] = _wifi->remainingSec();
    w["staConnected"] = _wifi->staConnected();
    w["staSsid"] = _wifi->staConnected() ? WiFi.SSID() : String("");
    w["staIp"] = _wifi->staIp();
    w["rssi"] = _wifi->staConnected() ? WiFi.RSSI() : 0;
    w["apRunning"] = _wifi->apRunning();
    w["apSsid"] = _wifi->apSsid();
    w["apIp"] = _wifi->apIp();
    w["apClients"] = _wifi->apClients();

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void RoverWebServer::handleGetConfig(AsyncWebServerRequest *request)
{
    StaticJsonDocument<512> doc;
    doc["transport"] = transportModeId(_settings->transport);
    doc["tcpPort"] = _settings->tcpPort;

    JsonObject w = doc.createNestedObject("wifi");
    w["mode"] = wifiModeId(_settings->wifiMode);
    w["staSsid"] = _settings->staSsid;
    w["staPasswordSet"] = _settings->staPassword.length() > 0;
    w["apSsid"] = _settings->apSsid;
    w["apPasswordSet"] = _settings->apPassword.length() > 0;
    w["windowSec"] = _settings->setupWindowSec;

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void RoverWebServer::handleSaveConfig(AsyncWebServerRequest *request, JsonVariant &json)
{
    JsonObject root = json.as<JsonObject>();
    if (root.isNull())
    {
        sendError(request, 400, "Invalid JSON");
        return;
    }

    // Start from the current values: missing keys stay unchanged
    RoverSettings next = *_settings;

    if (root.containsKey("transport"))
    {
        String id = root["transport"].as<String>();
        if (id == "ble")
            next.transport = TransportMode::BLE;
        else if (id == "spp")
            next.transport = TransportMode::SPP;
        else if (id == "tcp")
            next.transport = TransportMode::TCP;
        else
        {
            sendError(request, 400, "Unknown transport");
            return;
        }
    }
    if (root.containsKey("tcpPort"))
    {
        uint32_t port = root["tcpPort"].as<uint32_t>();
        if (port < 1 || port > 65535)
        {
            sendError(request, 400, "TCP port must be 1-65535");
            return;
        }
        next.tcpPort = (uint16_t)port;
    }

    JsonObject w = root["wifi"];
    if (!w.isNull())
    {
        if (w.containsKey("mode"))
        {
            String id = w["mode"].as<String>();
            if (id == "ap")
                next.wifiMode = WifiMode::AP;
            else if (id == "sta")
                next.wifiMode = WifiMode::STA;
            else if (id == "ap_sta")
                next.wifiMode = WifiMode::AP_STA;
            else
            {
                sendError(request, 400, "Unknown WiFi mode");
                return;
            }
        }
        if (w.containsKey("staSsid"))
            next.staSsid = w["staSsid"].as<String>();
        // Passwords are write-only: absent = keep, present (even "") = replace
        if (w.containsKey("staPassword"))
            next.staPassword = w["staPassword"].as<String>();
        if (w.containsKey("apSsid"))
            next.apSsid = w["apSsid"].as<String>();
        if (w.containsKey("apPassword"))
            next.apPassword = w["apPassword"].as<String>();
        if (w.containsKey("windowSec"))
        {
            uint32_t seconds = w["windowSec"].as<uint32_t>();
            if (seconds < SETUP_WINDOW_MIN_SEC || seconds > SETUP_WINDOW_MAX_SEC)
            {
                sendError(request, 400, "Setup window must be 15-600 seconds");
                return;
            }
            next.setupWindowSec = (uint16_t)seconds;
        }
    }

    String error;
    if (!next.validate(error))
    {
        sendError(request, 400, error.c_str());
        return;
    }

    if (!_store->save(next))
    {
        sendError(request, 500, "Failed to save settings");
        return;
    }

    *_settings = next;
    logPrintln("[Web] Settings saved - transport=%s wifi=%s, restarting", transportModeId(next.transport), wifiModeId(next.wifiMode));
    request->send(200, "application/json", "{\"success\":true,\"message\":\"Settings saved. Device will restart.\"}");
    scheduleRestart(1500);
}

void RoverWebServer::handleScan(AsyncWebServerRequest *request)
{
    // The STA interface has to exist for a scan
    if (WiFi.getMode() == WIFI_MODE_AP)
    {
        sendError(request, 400, "Scanning needs Station or AP+Station mode");
        return;
    }

    int16_t state = WiFi.scanComplete();
    if (state == WIFI_SCAN_FAILED)
    {
        WiFi.scanNetworks(true); // async, the UI polls this endpoint
        request->send(200, "application/json", "{\"success\":true,\"scanning\":true,\"networks\":[]}");
        return;
    }
    if (state == WIFI_SCAN_RUNNING)
    {
        request->send(200, "application/json", "{\"success\":true,\"scanning\":true,\"networks\":[]}");
        return;
    }

    DynamicJsonDocument doc(2048);
    doc["success"] = true;
    doc["scanning"] = false;
    JsonArray networks = doc.createNestedArray("networks");
    for (int i = 0; i < state && i < 15; i++)
    {
        JsonObject net = networks.createNestedObject();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["secured"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        net["channel"] = WiFi.channel(i);
    }
    WiFi.scanDelete();

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void RoverWebServer::handleGnssStatus(AsyncWebServerRequest *request)
{
    GnssStatus s;
    _gnss->snapshot(s);

    DynamicJsonDocument doc(1024);
    doc["state"] = gnssJobStateId(s.state);
    doc["op"] = gnssOpId(s.op);
    doc["step"] = s.step;
    doc["steps"] = s.steps;
    doc["needsReboot"] = s.needsReboot;
    doc["error"] = s.error;

    // Values are only meaningful after a completed read; -1 = not supported
    const GnssModuleState &m = s.module;
    if (m.valid)
    {
        JsonObject mod = doc.createNestedObject("module");
        mod["version"] = m.version;
        mod["variant"] = m.variant;
        mod["rcvrMode"] = m.rcvrMode;
        JsonArray cnst = mod.createNestedArray("constellation");
        for (int i = 0; i < GNSS_CNST_COUNT; i++)
            cnst.add(m.constellation[i]);
        mod["fixIntervalMs"] = m.fixIntervalMs;
        JsonObject msg = mod.createNestedObject("msg");
        for (int i = 0; i < GNSS_MSG_COUNT; i++)
            msg[GNSS_MSG_NAMES[i]] = m.msgRate[i];
        mod["sbas"] = m.sbas;
        mod["navMode"] = m.navMode;
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void RoverWebServer::handleGnssRequest(AsyncWebServerRequest *request, JsonVariant &json)
{
    JsonObject root = json.as<JsonObject>();
    if (root.isNull())
    {
        sendError(request, 400, "Invalid JSON");
        return;
    }

    String opId = root["op"] | "";
    GnssOp op = GnssOp::None;
    if (opId == "read")
        op = GnssOp::Read;
    else if (opId == "apply")
        op = GnssOp::Apply;
    else if (opId == "reboot")
        op = GnssOp::Reboot;
    else if (opId == "hot")
        op = GnssOp::HotStart;
    else if (opId == "warm")
        op = GnssOp::WarmStart;
    else if (opId == "cold")
        op = GnssOp::ColdStart;
    else if (opId == "restore")
        op = GnssOp::Restore;
    else
    {
        sendError(request, 400, "Unknown operation");
        return;
    }

    GnssChanges ch;
    ch.clear();

    if (op == GnssOp::Apply)
    {
        // Same limits as the protocol spec; the module has the final word
        JsonObject set = root["set"];
        if (set.isNull())
        {
            sendError(request, 400, "Nothing to apply");
            return;
        }
        if (set.containsKey("rcvrMode"))
        {
            int v = set["rcvrMode"] | -1;
            if (v != 1 && v != 2)
            {
                sendError(request, 400, "Receiver mode must be rover (1) or base (2)");
                return;
            }
            ch.rcvrMode = (int8_t)v;
        }
        if (set.containsKey("constellation"))
        {
            JsonArray arr = set["constellation"];
            int enabled = 0;
            if (arr.isNull() || arr.size() != GNSS_CNST_COUNT)
            {
                sendError(request, 400, "Constellation needs 6 flags");
                return;
            }
            for (int i = 0; i < GNSS_CNST_COUNT; i++)
            {
                int v = arr[i] | -1;
                if (v != 0 && v != 1)
                {
                    sendError(request, 400, "Constellation flags must be 0 or 1");
                    return;
                }
                ch.constellation[i] = (int8_t)v;
                enabled += v;
            }
            if (enabled == 0)
            {
                sendError(request, 400, "Enable at least one constellation");
                return;
            }
        }
        if (set.containsKey("fixIntervalMs"))
        {
            long v = set["fixIntervalMs"] | -1L;
            if (v != 100 && v != 200 && v != 500 && v != 1000)
            {
                sendError(request, 400, "Fix interval must be 100, 200, 500 or 1000 ms");
                return;
            }
            ch.fixIntervalMs = v;
        }
        JsonObject msg = set["msg"];
        if (!msg.isNull())
        {
            for (JsonPair kv : msg)
            {
                int index = -1;
                for (int i = 0; i < GNSS_MSG_COUNT; i++)
                    if (strcmp(kv.key().c_str(), GNSS_MSG_NAMES[i]) == 0)
                        index = i;
                int v = kv.value() | -1;
                if (index < 0 || v < 0 || v > 20)
                {
                    sendError(request, 400, "NMEA rates must be 0-20 for known sentences");
                    return;
                }
                ch.msgRate[index] = (int8_t)v;
            }
        }
        if (set.containsKey("sbas"))
        {
            int v = set["sbas"] | -1;
            if (v != 0 && v != 1)
            {
                sendError(request, 400, "SBAS must be 0 or 1");
                return;
            }
            ch.sbas = (int8_t)v;
        }
        if (set.containsKey("navMode"))
        {
            int v = set["navMode"] | -1;
            if (v != 0 && v != 1 && v != 4 && v != 5 && v != 7 && v != 9)
            {
                sendError(request, 400, "Unknown navigation mode");
                return;
            }
            ch.navMode = (int8_t)v;
        }
        if (ch.empty())
        {
            sendError(request, 400, "Nothing to apply");
            return;
        }
    }

    if (!_gnss->request(op, &ch))
    {
        sendError(request, 409, "The GNSS module is busy with another operation");
        return;
    }
    logPrintln("[Web] GNSS %s requested", gnssOpId(op));
    request->send(202, "application/json", "{\"success\":true}");
}
