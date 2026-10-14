// OSS RTK Rover web UI - plain JS, no build step.
// Talks to the REST API in firmware/rover/src/web_server.cpp.
(function () {
    'use strict';

    var STATUS_POLL_MS = 3000;
    var STATUS_TIMEOUT_MS = 5000;
    var STATUS_MAX_MISSES = 3;
    var HEARTBEAT_IDLE_MS = 5 * 60 * 1000; // stop holding WiFi on after this long without input
    var RECONNECT_FIRST_MS = 3000; // the rover restarts ~1.5s after answering
    var RECONNECT_POLL_MS = 1500;
    var RECONNECT_TIMEOUT_MS = 45000;
    var SCAN_POLL_MS = 1000;
    var SCAN_TIMEOUT_MS = 20000;
    var RESERVED_PORTS = [80, 23, 3232]; // HTTP, Telnet, OTA

    var TRANSPORT_LABEL = { ble: 'Bluetooth LE', spp: 'Bluetooth SPP', tcp: 'WiFi (TCP)' };
    var WIFI_MODE_LABEL = { ap: 'Access point', sta: 'Station', ap_sta: 'AP + Station' };
    var WIFI_MODE_HINT = {
        ap: 'The rover creates its own network. Connect your phone to it to reach this page.',
        sta: 'The rover joins an existing network. If it cannot connect within 10 seconds, it opens its own access point instead.',
        ap_sta: 'The rover joins an existing network and keeps its own access point available.'
    };

    var config = null; // last config read from the device
    var status = null; // last status read from the device
    var busy = false;  // saving / restarting / waiting for the device
    var statusMisses = 0;
    var statusInFlight = false;
    var lastInteraction = Date.now();
    var scanToken = 0; // invalidates a running scan when the dialog closes
    var currentTab = 'status';
    var barState = 'actions';

    function $(id) { return document.getElementById(id); }

    function radioValue(name) {
        var el = document.querySelector('input[name="' + name + '"]:checked');
        return el ? el.value : null;
    }

    function setRadio(name, value) {
        var el = document.querySelector('input[name="' + name + '"][value="' + value + '"]');
        if (el) el.checked = true;
    }

    function sleep(ms) {
        return new Promise(function (resolve) { setTimeout(resolve, ms); });
    }

    // ===== API =====

    function api(method, path, body, timeoutMs) {
        var controller = new AbortController();
        var timer = setTimeout(function () { controller.abort(); }, timeoutMs || 8000);
        var options = { method: method, signal: controller.signal, cache: 'no-store' };
        if (body !== undefined) {
            options.headers = { 'Content-Type': 'application/json' };
            options.body = JSON.stringify(body);
        }
        return fetch(path, options)
            .then(function (res) {
                return res.json().catch(function () { return {}; }).then(function (data) {
                    if (!res.ok) {
                        throw new Error(data.message || ('Request failed (' + res.status + ')'));
                    }
                    return data;
                });
            })
            .finally(function () { clearTimeout(timer); });
    }

    // ===== Toasts =====

    function toast(message, type) {
        var el = document.createElement('div');
        el.className = 'toast ' + (type || 'success');
        el.textContent = message;
        $('toasts').appendChild(el);
        setTimeout(function () { el.remove(); }, type === 'error' ? 6000 : 3500);
    }

    // ===== Tabs =====

    function showTab(name) {
        currentTab = name;
        document.querySelectorAll('.tab').forEach(function (tab) {
            var active = tab.dataset.tab === name;
            tab.setAttribute('aria-selected', active ? 'true' : 'false');
            $('panel-' + tab.dataset.tab).hidden = !active;
        });
        // The GNSS tab has its own actions: they talk to the module, not the rover settings
        if (barState === 'actions') setBar('actions');
        if (name === 'gnss' && !gnssLoaded && !busy) runGnss('read');
    }

    // ===== Status =====

    function formatDuration(totalSec) {
        var s = Math.max(0, Math.floor(totalSec));
        var d = Math.floor(s / 86400);
        var h = Math.floor((s % 86400) / 3600);
        var m = Math.floor((s % 3600) / 60);
        var sec = s % 60;
        if (d > 0) return d + 'd ' + h + 'h';
        if (h > 0) return h + 'h ' + m + 'min';
        if (m > 0) return m + 'min ' + sec + 's';
        return sec + 's';
    }

    function setValue(id, text, className) {
        var el = $(id);
        el.textContent = text;
        el.className = className || '';
    }

    function hostList() {
        var hosts = ['ossrtk.local'];
        if (status && status.wifi) {
            if (status.wifi.staIp) hosts.push(status.wifi.staIp);
            if (status.wifi.apIp) hosts.push(status.wifi.apIp);
        }
        return hosts;
    }

    function renderStatus() {
        var online = !!status;
        var pill = $('linkPill');
        $('offlineNote').hidden = online || busy;
        $('setupNote').hidden = !online || status.transport.active;

        if (!online) {
            pill.hidden = false;
            pill.className = 'pill off';
            pill.textContent = busy ? 'Restarting' : 'Offline';
            return;
        }

        var tr = status.transport;
        var w = status.wifi;

        // During the setup window only WiFi runs; the Bluetooth link starts after it
        var label = TRANSPORT_LABEL[tr.mode] || tr.name;
        pill.hidden = false;
        pill.className = 'pill' + (tr.connected ? ' on' : '');
        pill.textContent = !tr.active ? 'Setup mode' : (tr.connected ? 'Client connected' : 'Waiting for client');

        if (!tr.active) {
            $('setupText').textContent = holdingWifi()
                ? label + ' is off while this page is open. It starts when you close the page or press the button.'
                : label + ' starts in ' + formatDuration(w.remainingSec) + ' unless you use this page.';
        }

        setValue('stTransport', label);
        if (!tr.active) {
            setValue('stClient', 'Starts when WiFi turns off', 'dim');
        } else if (tr.connected) {
            setValue('stClient', tr.clients > 1 ? tr.clients + ' connected' : 'Connected', 'ok');
        } else {
            setValue('stClient', 'Not connected', 'dim');
        }
        $('stTcpRow').hidden = tr.mode !== 'tcp';
        if (tr.mode === 'tcp' && config) {
            $('stTcpAddr').textContent = hostList().map(function (h) { return h + ':' + config.tcpPort; }).join(', ');
        }

        if (!w.enabled) {
            setValue('stWifiMode', 'Off', 'dim');
        } else {
            setValue('stWifiMode', WIFI_MODE_LABEL[w.mode] || w.mode);
        }
        if (w.staConnected) {
            setValue('stSta', w.staSsid + '\n' + w.staIp + ' · ' + w.rssi + ' dBm');
        } else {
            setValue('stSta', w.mode === 'ap' ? 'Not used' : 'Not connected', 'dim');
        }
        if (w.apRunning) {
            setValue('stAp', w.apSsid + '\n' + w.apIp + ' · ' +
                w.apClients + (w.apClients === 1 ? ' client' : ' clients'));
        } else {
            setValue('stAp', 'Off', 'dim');
        }
        if (w.remainingSec < 0) {
            setValue('stRemaining', 'Stays on');
        } else {
            setValue('stRemaining', holdingWifi() ? 'When this page is closed' : formatDuration(w.remainingSec));
        }

        setValue('stVersion', status.version);
        setValue('stUptime', formatDuration(status.uptime));
        setValue('stHeap', Math.round(status.freeHeap / 1024) + ' kB');

        updateHints();
    }

    // The rover keeps WiFi up (and Bluetooth off) only while a page is really
    // in use: visible, and touched within the last few minutes. A forgotten tab
    // or a phone that merely joined the WiFi must not block the Bluetooth link.
    function holdingWifi() {
        return !document.hidden && (Date.now() - lastInteraction) < HEARTBEAT_IDLE_MS;
    }

    function pollStatus() {
        if (busy || statusInFlight) return Promise.resolve();
        statusInFlight = true;
        // Only call the rover offline after several misses in a row
        return api('GET', '/api/status' + (holdingWifi() ? '?ka=1' : ''), undefined, STATUS_TIMEOUT_MS)
            .then(function (data) {
                status = data;
                statusMisses = 0;
            })
            .catch(function () {
                if (++statusMisses >= STATUS_MAX_MISSES) status = null;
            })
            .then(function () {
                statusInFlight = false;
                if (!busy) renderStatus();
            });
    }

    function finishSetup() {
        $('finishBtn').disabled = true;
        api('POST', '/api/wifi/off')
            .then(function () {
                status = null;
                statusMisses = STATUS_MAX_MISSES;
                renderStatus();
                toast('WiFi is turning off. Bluetooth starts in a moment.');
            })
            .catch(function (err) { toast('Could not finish setup: ' + err.message, 'error'); })
            .then(function () { $('finishBtn').disabled = false; });
    }

    // ===== Form =====

    function updateHints() {
        var hosts = hostList();
        var port = $('tcpPort').value || (config ? config.tcpPort : '');
        $('tcpHint').textContent = hosts.map(function (h) { return h + ':' + port; }).join(' or ');

        // The running AP name is the automatic one whenever no custom name is saved
        var auto = '';
        if (config && !config.wifi.apSsid && status && status.wifi.apSsid) {
            auto = ' (' + status.wifi.apSsid + ')';
        }
        $('apAutoName').textContent = auto;
    }

    function updateVisibility() {
        var transport = radioValue('transport');
        var mode = radioValue('wifiMode');

        $('tcpFields').hidden = transport !== 'tcp';
        $('staFields').hidden = mode === 'ap';
        $('apFields').hidden = mode === 'sta';
        $('staRequired').hidden = mode !== 'sta';
        $('wifiModeHint').textContent = WIFI_MODE_HINT[mode] || '';

        $('staPassword').disabled = $('staOpen').checked;
        $('apPassword').disabled = $('apOpen').checked;

        var tcp = transport === 'tcp';
        $('windowSec').disabled = tcp;
        $('activeHint').textContent = tcp
            ? 'Not used with the WiFi (TCP) link - WiFi stays on so apps can connect.'
            : 'After power-on the rover runs WiFi only, for setup and firmware updates. If nobody opens ' +
              'this page (or starts an OTA / Telnet session) within this time, WiFi turns off and ' +
              'Bluetooth starts. Restart the rover to get WiFi back.';

        updateHints();
    }

    function fillForm() {
        var w = config.wifi;
        setRadio('transport', config.transport);
        $('tcpPort').value = config.tcpPort;
        setRadio('wifiMode', w.mode);
        $('staSsid').value = w.staSsid;
        $('apSsid').value = w.apSsid;
        $('windowSec').value = w.windowSec;

        // Passwords are write-only on the device: an empty field keeps the saved one
        $('staPassword').value = '';
        $('apPassword').value = '';
        $('staPassword').placeholder = w.staPasswordSet ? 'Saved - leave empty to keep' : 'Network password';
        $('apPassword').placeholder = w.apPasswordSet ? 'Saved - leave empty to keep' : 'At least 8 characters';
        $('staOpen').checked = !w.staPasswordSet;
        $('apOpen').checked = !w.apPasswordSet;

        clearInvalid();
        updateVisibility();
        renderGnss(); // the TCP note on the GNSS tab depends on the transport
    }

    function clearInvalid() {
        document.querySelectorAll('.input.invalid').forEach(function (el) { el.classList.remove('invalid'); });
    }

    // Shows the field (switching tab if needed) and reports the problem
    function invalid(tab, id, message) {
        showTab(tab);
        var el = $(id);
        el.classList.add('invalid');
        el.focus();
        toast(message, 'error');
        return null;
    }

    function parseIntField(id) {
        var raw = $(id).value.trim();
        return /^\d+$/.test(raw) ? parseInt(raw, 10) : NaN;
    }

    // Builds the POST /api/config body, or returns null after flagging a field.
    // Mirrors RoverSettings::validate() so mistakes are caught before the round trip.
    function readForm() {
        clearInvalid();

        var transport = radioValue('transport');
        var mode = radioValue('wifiMode');
        var body = { transport: transport, wifi: { mode: mode } };

        if (transport === 'tcp') {
            var port = parseIntField('tcpPort');
            if (isNaN(port) || port < 1 || port > 65535) {
                return invalid('connection', 'tcpPort', 'TCP port must be between 1 and 65535');
            }
            if (RESERVED_PORTS.indexOf(port) !== -1) {
                return invalid('connection', 'tcpPort', 'Port ' + port + ' is used by the rover (HTTP, Telnet or OTA)');
            }
            body.tcpPort = port;
        }

        if (mode !== 'ap') {
            var staSsid = $('staSsid').value;
            if (mode === 'sta' && staSsid.length === 0) {
                return invalid('wifi', 'staSsid', 'Station mode needs a network name (SSID)');
            }
            body.wifi.staSsid = staSsid;

            var staPassword = $('staPassword').value;
            if ($('staOpen').checked) {
                body.wifi.staPassword = '';
            } else if (staPassword.length > 0) {
                body.wifi.staPassword = staPassword;
            } else if (staSsid.length > 0 && (!config.wifi.staPasswordSet || staSsid !== config.wifi.staSsid)) {
                return invalid('wifi', 'staPassword', 'Enter the network password or mark the network as open');
            }
        }

        if (mode !== 'sta') {
            body.wifi.apSsid = $('apSsid').value;

            var apPassword = $('apPassword').value;
            if ($('apOpen').checked) {
                body.wifi.apPassword = '';
            } else if (apPassword.length > 0) {
                if (apPassword.length < 8) {
                    return invalid('wifi', 'apPassword', 'Access point password must be at least 8 characters');
                }
                body.wifi.apPassword = apPassword;
            } else if (!config.wifi.apPasswordSet) {
                return invalid('wifi', 'apPassword', 'Enter an access point password or mark it as open');
            }
        }

        if (transport !== 'tcp') {
            var seconds = parseIntField('windowSec');
            if (isNaN(seconds) || seconds < 15 || seconds > 600) {
                return invalid('wifi', 'windowSec', 'Setup window must be between 15 and 600 seconds');
            }
            body.wifi.windowSec = seconds;
        }

        return body;
    }

    function loadConfig(quiet) {
        return api('GET', '/api/config')
            .then(function (data) {
                config = data;
                fillForm();
                if (!quiet) toast('Settings reloaded');
                return true;
            })
            .catch(function (err) {
                toast('Could not load settings: ' + err.message, 'error');
                return false;
            });
    }

    // ===== Save / restart / reconnect =====

    function setBar(state, text) {
        barState = state;
        $('barActions').hidden = state !== 'actions' || currentTab === 'gnss';
        $('barGnss').hidden = state !== 'actions' || currentTab !== 'gnss';
        $('barBusy').hidden = state !== 'busy';
        $('barLost').hidden = state !== 'lost';
        if (text) $('busyText').textContent = text;
    }

    function setBusy(value, text) {
        busy = value;
        setBar(value ? 'busy' : 'actions', text);
        if (value) {
            status = null;
            renderStatus();
        }
    }

    function waitForDevice() {
        var deadline = Date.now() + RECONNECT_TIMEOUT_MS;

        function attempt() {
            return api('GET', '/api/status', undefined, 2000).catch(function () {
                if (Date.now() >= deadline) return null;
                return sleep(RECONNECT_POLL_MS).then(attempt);
            });
        }

        return sleep(RECONNECT_FIRST_MS).then(attempt);
    }

    function reconnect(successMessage) {
        setBusy(true, 'Reconnecting to device...');
        return waitForDevice().then(function (data) {
            if (!data) {
                busy = false;
                setBar('lost');
                renderStatus();
                return;
            }
            status = data;
            setBusy(false);
            renderStatus();
            return loadConfig(true).then(function () {
                renderStatus(); // the TCP address row depends on the fresh config
                toast(successMessage);
            });
        });
    }

    function save(body) {
        setBusy(true, 'Saving...');
        api('POST', '/api/config', body)
            .then(function () { return reconnect('Configuration saved. Device reconnected.'); })
            .catch(function (err) {
                setBusy(false);
                toast('Failed to save: ' + err.message, 'error');
                pollStatus();
            });
    }

    function restart() {
        setBusy(true, 'Restarting...');
        api('POST', '/api/restart')
            .then(function () { return reconnect('Device restarted.'); })
            .catch(function (err) {
                setBusy(false);
                toast('Failed to restart: ' + err.message, 'error');
                pollStatus();
            });
    }

    // ===== Confirm dialog =====

    var confirmAction = null;

    function confirmDialog(title, text, okLabel, action) {
        $('confirmTitle').textContent = title;
        $('confirmText').textContent = text;
        $('confirmOk').textContent = okLabel;
        confirmAction = action;
        $('confirmDialog').showModal();
    }

    // What the user is about to lose access to, so the restart is not a surprise
    function saveWarning(body) {
        var text = 'This will save the configuration and restart the device. ' +
            'It will be unavailable for a few seconds.';
        var w = config.wifi;
        var wifiChanged = body.wifi.mode !== w.mode ||
            (body.wifi.staSsid !== undefined && body.wifi.staSsid !== w.staSsid) ||
            body.wifi.staPassword !== undefined ||
            (body.wifi.apSsid !== undefined && body.wifi.apSsid !== w.apSsid) ||
            body.wifi.apPassword !== undefined;
        if (wifiChanged) {
            text += ' WiFi settings changed, so you may need to reconnect to the rover on its new network.';
        }
        return text;
    }

    // ===== WiFi scan =====

    function signalBars(rssi) {
        if (rssi >= -50) return '▂▄▆█';
        if (rssi >= -60) return '▂▄▆';
        if (rssi >= -70) return '▂▄';
        return '▂';
    }

    function svgIcon(id) {
        var ns = 'http://www.w3.org/2000/svg';
        var svg = document.createElementNS(ns, 'svg');
        svg.setAttribute('class', 'icon');
        var use = document.createElementNS(ns, 'use');
        use.setAttribute('href', '#' + id);
        svg.appendChild(use);
        return svg;
    }

    function scanMessage(text) {
        var list = $('scanList');
        list.textContent = '';
        var el = document.createElement('div');
        el.className = 'networks-empty';
        el.textContent = text;
        list.appendChild(el);
    }

    function setScanning(value) {
        $('rescanBtn').disabled = value;
        $('rescanBtn').querySelector('.icon').classList.toggle('spin', value);
        $('rescanText').textContent = value ? 'Scanning...' : 'Rescan';
    }

    function pickNetwork(net) {
        $('staSsid').value = net.ssid;
        $('staPassword').value = '';
        $('staOpen').checked = !net.secured;
        $('scanDialog').close();
        updateVisibility();
        if (net.secured) $('staPassword').focus();
    }

    function renderNetworks(networks) {
        // Strongest first; one row per SSID (mesh networks repeat it), hidden ones skipped
        var seen = {};
        var rows = networks
            .slice()
            .sort(function (a, b) { return b.rssi - a.rssi; })
            .filter(function (net) {
                if (!net.ssid || seen[net.ssid]) return false;
                seen[net.ssid] = true;
                return true;
            });

        if (rows.length === 0) {
            scanMessage('No networks found');
            return;
        }

        var list = $('scanList');
        list.textContent = '';
        rows.forEach(function (net) {
            var row = document.createElement('button');
            row.type = 'button';
            row.className = 'network';
            row.appendChild(svgIcon('i-wifi'));

            var main = document.createElement('div');
            main.className = 'network-main';
            var name = document.createElement('div');
            name.className = 'network-name';
            var ssid = document.createElement('span');
            ssid.textContent = net.ssid;
            name.appendChild(ssid);
            if (net.secured) name.appendChild(svgIcon('i-lock'));
            var sub = document.createElement('div');
            sub.className = 'network-sub';
            sub.textContent = 'Ch ' + net.channel + (net.secured ? '' : ' · Open');
            main.appendChild(name);
            main.appendChild(sub);
            row.appendChild(main);

            var signal = document.createElement('div');
            signal.className = 'network-signal';
            var bars = document.createElement('div');
            bars.className = 'network-bars';
            bars.textContent = signalBars(net.rssi);
            var rssi = document.createElement('div');
            rssi.className = 'network-rssi';
            rssi.textContent = net.rssi + ' dBm';
            signal.appendChild(bars);
            signal.appendChild(rssi);
            row.appendChild(signal);

            row.addEventListener('click', function () { pickNetwork(net); });
            list.appendChild(row);
        });
    }

    function scan() {
        var token = ++scanToken;
        var deadline = Date.now() + SCAN_TIMEOUT_MS;
        setScanning(true);
        scanMessage('Scanning...');

        function poll() {
            return api('GET', '/api/wifi/scan').then(function (data) {
                if (token !== scanToken) return null;
                if (!data.scanning) return data.networks || [];
                if (Date.now() >= deadline) throw new Error('Scan timed out. Try again.');
                return sleep(SCAN_POLL_MS).then(poll);
            });
        }

        poll()
            .then(function (networks) {
                if (token !== scanToken || networks === null) return;
                renderNetworks(networks);
            })
            .catch(function (err) {
                if (token !== scanToken) return;
                scanMessage(err.message);
            })
            .then(function () {
                if (token === scanToken) setScanning(false);
            });
    }

    // ===== GNSS module (LC29H) =====
    // Settings live in the module itself. The rover runs each request as a
    // short command job (GET /api/gnss reports progress) and the form always
    // shows what the module answered, never what was sent.

    var GNSS_POLL_MS = 400;
    var GNSS_TIMEOUT_MS = 60000;
    // $PQTMCFGCNST combinations the RTK variants (BA, CA, DA, EA) accept.
    // Order: GPS, GLONASS, Galileo, BeiDou, QZSS, NavIC.
    var CNST_OPTIONS = [
        ['1,1,1,1,1,0', 'GPS + GLONASS + Galileo + BeiDou + QZSS'],
        ['1,0,1,1,1,0', 'GPS + Galileo + BeiDou + QZSS'],
        ['1,0,1,0,1,0', 'GPS + Galileo + QZSS'],
        ['1,0,0,1,1,0', 'GPS + BeiDou + QZSS'],
        ['1,0,0,1,0,0', 'GPS + BeiDou'],
        ['1,0,0,0,1,0', 'GPS + QZSS'],
        ['0,0,0,1,0,0', 'BeiDou'],
        ['1,0,0,0,0,0', 'GPS']
    ];
    var CNST_NAMES = ['GPS', 'GLONASS', 'Galileo', 'BeiDou', 'QZSS', 'NavIC'];
    var MSG_RATES = [[0, 'Off'], [1, 'Every fix'], [2, 'Every 2nd fix'], [5, 'Every 5th fix'], [10, 'Every 10th fix']];
    var NAV_MODES = [[0, 'Normal (vehicle)'], [1, 'Pedestrian (walking with a pole)'], [4, 'Stationary (static points)'],
        [5, 'Drone'], [9, 'Bike']];
    var FIX_RATES = [[1000, '1 Hz'], [500, '2 Hz'], [200, '5 Hz'], [100, '10 Hz']];
    var VARIANT_INFO = {
        AA: 'Standard precision', BA: 'RTK + dead reckoning (IMU)', CA: 'Dead reckoning (IMU)',
        DA: 'RTK, 1 Hz', EA: 'RTK, up to 10 Hz'
    };
    var GNSS_OP_TEXT = {
        read: 'Reading from module', apply: 'Writing to module', reboot: 'Rebooting module',
        hot: 'Hot start', warm: 'Warm start', cold: 'Cold start', restore: 'Restoring factory settings'
    };
    var GNSS_OP_DONE = {
        read: 'GNSS settings read from the module',
        apply: 'Settings saved in the GNSS module',
        reboot: 'GNSS module rebooted',
        hot: 'Hot start done',
        warm: 'Warm start done',
        cold: 'Cold start done. The first fix takes about 30 s.',
        restore: 'Factory settings restored. Reboot the module to apply them.'
    };
    var GNSS_CONFIRM = {
        hot: ['Hot start?', 'Restarts positioning with the stored satellite data. The fix drops for a moment.', 'Hot start'],
        warm: ['Warm start?', 'Discards the ephemeris. The next fix takes about 15 seconds.', 'Warm start'],
        cold: ['Cold start?', 'Discards time, position and satellite data. The next fix takes about 30 seconds.', 'Cold start'],
        reboot: ['Reboot GNSS module?', 'Restarts the LC29H so saved settings take effect. If they still do not ' +
            'show up afterwards, switch the rover off and on.', 'Reboot'],
        restore: ['Restore factory settings?', 'Resets receiver mode, constellations, position rate and NMEA output ' +
            'of the LC29H to factory values. They apply after a module reboot.', 'Restore']
    };

    var gnss = null;        // last GET /api/gnss
    var gnssModule = null;  // values the form was filled from
    var gnssBusy = false;   // a job started from this page is running
    var gnssLoaded = false; // a read was started since the page loaded

    // Fills a <select>; a current value outside the list is kept as an extra
    // option, so reading and re-applying never changes it by accident
    function selectOptions(select, options, current, currentLabel) {
        select.textContent = '';
        var found = false;
        options.forEach(function (o) {
            var el = document.createElement('option');
            el.value = String(o[0]);
            el.textContent = o[1];
            if (String(o[0]) === String(current)) found = true;
            select.appendChild(el);
        });
        if (!found) {
            var extra = document.createElement('option');
            extra.value = String(current);
            extra.textContent = currentLabel || ('Current (' + current + ')');
            select.appendChild(extra);
        }
        select.value = String(current);
    }

    function cnstLabel(flags) {
        var names = [];
        flags.forEach(function (on, i) { if (on > 0) names.push(CNST_NAMES[i]); });
        return names.join(' + ') + ' (current)';
    }

    function renderGnss() {
        var g = gnss;
        var running = gnssBusy || !!(g && g.state === 'busy');

        if (running) {
            var steps = g && g.steps ? ' (' + g.step + '/' + g.steps + ')' : '';
            setValue('gnState', (GNSS_OP_TEXT[g && g.op] || 'Working') + '...' + steps, 'dim');
        } else if (g && g.state === 'failed') {
            setValue('gnState', 'Last operation failed');
        } else if (gnssModule) {
            setValue('gnState', 'Ready', 'ok');
        } else {
            setValue('gnState', 'Not read yet', 'dim');
        }

        var failed = !running && !!(g && g.state === 'failed');
        $('gnError').hidden = !failed;
        $('gnError').textContent = failed ? g.error : '';
        $('gnRebootNote').hidden = running || !(g && g.needsReboot);
        $('gnForm').hidden = !gnssModule;

        document.querySelectorAll('#barGnss button, [data-gnss-op], #gnRebootNoteBtn').forEach(function (b) {
            b.disabled = running;
        });
        $('gnApplyBtn').disabled = running || !gnssModule;

        $('gnTcpHint').textContent = config && config.transport === 'tcp'
            ? 'GNSS data to TCP clients pauses for a moment while the rover talks to the module.'
            : '';
    }

    function updateGnssHints() {
        var base = radioValue('gnRcvr') === '2';
        $('gnRcvrHint').textContent = base
            ? 'Base mode turns NMEA off and outputs RTCM corrections instead, so survey apps get no position. ' +
              'Survey-in and fixed base coordinates are not configured here.'
            : 'Normal mode for this device: positions out, corrections in.';

        if (gnssModule && gnssModule.variant === 'DA') {
            $('gnRateHint').textContent = 'Experimental: the datasheet rates the LC29H (DA) at 1 Hz only and the ' +
                'protocol spec does not list this command for it. The module may reject it or ignore it after a reboot.';
        } else if (parseInt($('gnRate').value, 10) < 1000) {
            $('gnRateHint').textContent = 'Above 1 Hz only GGA and RMC follow this rate, GSA and GSV stay at 1 Hz. ' +
                'Check that your Bluetooth link keeps up.';
        } else {
            $('gnRateHint').textContent = 'How often the module computes and reports a position.';
        }
    }

    function fillGnss(m) {
        gnssModule = m;

        var info = VARIANT_INFO[m.variant];
        setValue('gnModel', m.variant ? 'LC29H (' + m.variant + ')' + (info ? '\n' + info : '') : 'Unknown');
        setValue('gnVersion', m.version || '\u2013', 'mono');

        $('gnRcvrField').hidden = m.rcvrMode < 1;
        if (m.rcvrMode > 0) setRadio('gnRcvr', String(m.rcvrMode));

        $('gnCnstField').hidden = m.constellation[0] < 0;
        if (m.constellation[0] >= 0) {
            selectOptions($('gnCnst'), CNST_OPTIONS, m.constellation.join(','), cnstLabel(m.constellation));
        }

        // The datasheet rates the DA at 1 Hz only, but the field stays editable
        // there for testing. If the module did not report its rate, "Unknown"
        // is shown and any pick counts as a change.
        $('gnRateField').hidden = m.fixIntervalMs <= 0 && m.variant !== 'DA';
        if (m.fixIntervalMs > 0) {
            selectOptions($('gnRate'), FIX_RATES, m.fixIntervalMs);
        } else {
            selectOptions($('gnRate'), FIX_RATES, -1, 'Unknown (module did not report it)');
        }
        $('gnRate').disabled = false;

        $('gnNavField').hidden = m.navMode < 0;
        if (m.navMode >= 0) selectOptions($('gnNav'), NAV_MODES, m.navMode);

        $('gnSbasField').hidden = m.sbas < 0;
        $('gnSbas').checked = m.sbas === 1;

        // One picker per sentence the module answered for
        var list = $('gnMsgList');
        list.textContent = '';
        var shown = 0;
        Object.keys(m.msg).forEach(function (name) {
            if (m.msg[name] < 0) return;
            var cell = document.createElement('div');
            var label = document.createElement('label');
            label.className = 'label';
            label.htmlFor = 'gnMsg-' + name;
            label.textContent = name;
            var select = document.createElement('select');
            select.className = 'input';
            select.id = 'gnMsg-' + name;
            selectOptions(select, MSG_RATES, m.msg[name], 'Every ' + m.msg[name] + ' fixes');
            cell.appendChild(label);
            cell.appendChild(select);
            list.appendChild(cell);
            shown++;
        });
        $('gnMsgField').hidden = shown === 0;

        updateGnssHints();
    }

    // Only what differs from the last read goes to the module
    function readGnssForm() {
        var m = gnssModule;
        var set = {};
        var changed = false;

        if (!$('gnRcvrField').hidden) {
            var rcvr = parseInt(radioValue('gnRcvr'), 10);
            if (rcvr !== m.rcvrMode) { set.rcvrMode = rcvr; changed = true; }
        }
        if (!$('gnCnstField').hidden && $('gnCnst').value !== m.constellation.join(',')) {
            set.constellation = $('gnCnst').value.split(',').map(Number);
            changed = true;
        }
        if (!$('gnRateField').hidden) {
            var interval = parseInt($('gnRate').value, 10);
            if (interval > 0 && interval !== m.fixIntervalMs) { set.fixIntervalMs = interval; changed = true; }
        }
        if (!$('gnNavField').hidden) {
            var nav = parseInt($('gnNav').value, 10);
            if (nav !== m.navMode) { set.navMode = nav; changed = true; }
        }
        if (!$('gnSbasField').hidden) {
            var sbas = $('gnSbas').checked ? 1 : 0;
            if (sbas !== m.sbas) { set.sbas = sbas; changed = true; }
        }
        var msg = {};
        var msgChanged = false;
        Object.keys(m.msg).forEach(function (name) {
            var el = $('gnMsg-' + name);
            if (!el || m.msg[name] < 0) return;
            var rate = parseInt(el.value, 10);
            if (rate !== m.msg[name]) { msg[name] = rate; msgChanged = true; }
        });
        if (msgChanged) { set.msg = msg; changed = true; }

        return changed ? set : null;
    }

    function applyWarning(set) {
        var text = 'The changes are written to the LC29H and saved in its memory.';
        if (set.rcvrMode !== undefined || set.constellation || set.fixIntervalMs !== undefined) {
            text += ' Receiver mode, constellations and position rate take effect after a module reboot.';
        }
        if (set.rcvrMode === 2) {
            text += ' Base mode stops NMEA output: survey apps will no longer get a position.';
        }
        if (set.msg && set.msg.GGA === 0) {
            text += ' GGA is turned off. Most survey and NTRIP apps need it.';
        }
        return text;
    }

    function runGnss(op, set) {
        if (gnssBusy) return Promise.resolve();
        gnssBusy = true;
        if (op === 'read') gnssLoaded = true;
        gnss = { state: 'busy', op: op, step: 0, steps: 0, needsReboot: !!(gnss && gnss.needsReboot) };
        renderGnss();

        var deadline = Date.now() + GNSS_TIMEOUT_MS;
        function poll() {
            return api('GET', '/api/gnss', undefined, 4000).then(function (data) {
                gnss = data;
                renderGnss();
                if (data.state !== 'busy') return data;
                if (Date.now() >= deadline) throw new Error('the module did not finish in time');
                return sleep(GNSS_POLL_MS).then(poll);
            });
        }

        var body = { op: op };
        if (set) body.set = set;
        return api('POST', '/api/gnss', body)
            .then(function () { return sleep(GNSS_POLL_MS).then(poll); })
            .then(function (data) {
                // A failed job can still carry values: the ones the module did answer
                if (data.module) fillGnss(data.module);
                if (data.state === 'done') {
                    toast(GNSS_OP_DONE[op] || 'Done');
                } else {
                    toast(data.error || 'GNSS operation failed', 'error');
                }
            })
            .catch(function (err) {
                gnss = { state: 'failed', op: op, error: 'GNSS: ' + err.message, needsReboot: !!(gnss && gnss.needsReboot) };
                toast('GNSS: ' + err.message, 'error');
            })
            .then(function () {
                gnssBusy = false;
                renderGnss();
            });
    }

    function gnssAction(op) {
        var c = GNSS_CONFIRM[op];
        confirmDialog(c[0], c[1], c[2], function () { runGnss(op); });
    }

    // ===== Wiring =====

    function init() {
        document.querySelectorAll('.tab').forEach(function (tab) {
            tab.addEventListener('click', function () { showTab(tab.dataset.tab); });
        });

        document.querySelectorAll('input[name="transport"], input[name="wifiMode"], #staOpen, #apOpen')
            .forEach(function (el) { el.addEventListener('change', updateVisibility); });
        $('tcpPort').addEventListener('input', updateHints);
        document.querySelectorAll('.input').forEach(function (el) {
            el.addEventListener('input', function () { el.classList.remove('invalid'); });
        });

        $('saveBtn').addEventListener('click', function () {
            if (!config) {
                toast('Settings are not loaded yet', 'error');
                return;
            }
            var body = readForm();
            if (!body) return;
            confirmDialog('Save Configuration?', saveWarning(body), 'Save & Restart', function () { save(body); });
        });

        $('reloadBtn').addEventListener('click', function () {
            loadConfig(false);
            pollStatus();
        });

        $('restartBtn').addEventListener('click', function () {
            confirmDialog('Restart Device?',
                'This will restart the device without saving changes. It will be unavailable for a few seconds.',
                'Restart', restart);
        });

        $('gnReadBtn').addEventListener('click', function () { runGnss('read'); });
        $('gnApplyBtn').addEventListener('click', function () {
            if (!gnssModule) return;
            var set = readGnssForm();
            if (!set) {
                toast('No changes to apply');
                return;
            }
            confirmDialog('Apply to GNSS module?', applyWarning(set), 'Apply', function () { runGnss('apply', set); });
        });
        $('gnRebootNoteBtn').addEventListener('click', function () { gnssAction('reboot'); });
        document.querySelectorAll('[data-gnss-op]').forEach(function (btn) {
            btn.addEventListener('click', function () { gnssAction(btn.dataset.gnssOp); });
        });
        document.querySelectorAll('input[name="gnRcvr"], #gnRate').forEach(function (el) {
            el.addEventListener('change', updateGnssHints);
        });
        renderGnss();

        $('finishBtn').addEventListener('click', finishSetup);
        ['pointerdown', 'keydown', 'input'].forEach(function (type) {
            document.addEventListener(type, function () { lastInteraction = Date.now(); }, true);
        });

        $('retryBtn').addEventListener('click', function () { reconnect('Device reconnected.'); });

        $('confirmCancel').addEventListener('click', function () { $('confirmDialog').close(); });
        $('confirmOk').addEventListener('click', function () {
            $('confirmDialog').close();
            if (confirmAction) confirmAction();
        });

        $('scanBtn').addEventListener('click', function () {
            $('scanDialog').showModal();
            scan();
        });
        $('rescanBtn').addEventListener('click', scan);
        $('scanClose').addEventListener('click', function () { $('scanDialog').close(); });
        $('scanDialog').addEventListener('close', function () {
            scanToken++; // drop the result of a scan still in flight
            setScanning(false);
        });

        // Click on the backdrop closes a dialog
        document.querySelectorAll('dialog').forEach(function (dialog) {
            dialog.addEventListener('click', function (event) {
                if (event.target === dialog) dialog.close();
            });
        });

        loadConfig(true).then(pollStatus);
        setInterval(function () {
            // No polling from a background tab: it would only burn rover battery
            if (!document.hidden) pollStatus();
        }, STATUS_POLL_MS);
        document.addEventListener('visibilitychange', function () {
            if (!document.hidden) pollStatus();
        });
    }

    init();
})();
