/**
 * SmartJunction AI - Master HUD Logic (v4.0)
 * Logic for Futuristic Single-Junction Focused Dashboard
 */

// --- System State ---
const state = {
    socket: null,
    selectedJct: 'J1',
    allJunctions: {},
    connected: false,
    alerts: [],
    junctions: ['J1', 'J2', 'J3', 'J4']
};

// --- DOM Cache (Focus HUD) ---
const elements = {
    status: document.getElementById('connection-status'),
    statusDot: document.getElementById('status-dot'),
    log: document.getElementById('system-log'),
    alertCountBadge: document.getElementById('alert-count'),
    networkList: document.getElementById('network-list'),
    
    // HUD Focused Elements
    metaFocus: document.getElementById('meta-focus'),
    streamFocus: document.getElementById('stream-focus'),
    placeholderFocus: document.getElementById('placeholder-focus'),
    svFocus: document.getElementById('sv-focus'),
    spFocus: document.getElementById('sp-focus'),
    sdFocus: document.getElementById('sd-focus'),
    tsFocus: document.getElementById('ts-focus'),
    barFocus: document.getElementById('bar-focus'),
    dangerHud: document.getElementById('danger-hud'),
    detsFocus: document.getElementById('dets-focus')
};

// --- Initialization ---
document.addEventListener('DOMContentLoaded', () => {
    addLogEntry('>> SYSTEM BOOT SEQUENCE INITIATED');
    addLogEntry('>> LOADING HUD OVERLAYS...');
    initSocket();
    
    // Fallback simulation
    setTimeout(() => {
        if (!state.connected) {
            addLogEntry('>> ! ERROR: EDGE_SERVER_TIMEOUT');
            addLogEntry('>> ! ACTIVATING LOCAL_SIM_PROTOCOL');
            startSimulation();
        }
    }, 4000);
});

/**
 * HUD Switching Logic
 */
function switchJunction(jctId) {
    state.selectedJct = jctId;
    elements.metaFocus.textContent = `SOURCE: ${jctId}`;
    addLogEntry(`>> MONITOR_RELOCATED: FOCUSING ON ${jctId}`);
    
    // Quick flash effect for visual feedback
    document.body.style.opacity = '0.7';
    setTimeout(() => document.body.style.opacity = '1', 100);
    
    updateUI();
}

/**
 * WebSocket Link
 */
function initSocket() {
    // Determine server URL dynamically
    const serverUrl = `${window.location.protocol}//${window.location.hostname}:5000`;
    state.socket = io(serverUrl, { timeout: 10000, reconnection: true });

    state.socket.on('connect', () => {
        state.connected = true;
        elements.status.textContent = 'LINK_ACTIVE';
        elements.statusDot.className = 'status-dot online';
        addLogEntry('>> ✓ SECURE HANDSHAKE: EDGE_SERVER_CONNECTED');
    });

    state.socket.on('disconnect', () => {
        state.connected = false;
        elements.status.textContent = 'LINK_LOST';
        elements.statusDot.className = 'status-dot offline';
        addLogEntry('>> ! WARNING: CONNECTION_INTERRUPTED_RETREIVING_LINK');
    });

    state.socket.on('state_update', (snapshot) => {
        state.allJunctions = snapshot;
        updateUI();
        updateNetworkList();
    });

    state.socket.on('new_alert', (alert) => {
        state.alerts.unshift(alert);
        if (state.alerts.length > 50) state.alerts.pop();
        addLogEntry(`${getAlertSymbol(alert.type)} ${alert.type} @ ${alert.source}: ${alert.details.toUpperCase()}`);
    });
}

/**
 * HUD UI Refresh
 */
function updateUI() {
    const data = state.allJunctions[state.selectedJct];
    if (!data) return;

    // Data Telemetry
    elements.svFocus.textContent = data.vehicles || 0;
    elements.spFocus.textContent = data.persons || 0;
    elements.sdFocus.textContent = (data.dist === undefined || data.dist === 999) ? '--' : `${data.dist}`;
    elements.tsFocus.textContent = `${(data.traffic_score || 0)}%`;
    
    const score = data.traffic_score || 0;
    elements.barFocus.style.width = score + '%';
    elements.barFocus.style.background = score > 80 ? 'var(--ruby-glow)' : (score > 50 ? 'var(--gold-glow)' : 'var(--emerald-glow)');
    elements.barFocus.style.boxShadow = `0 0 10px ${elements.barFocus.style.background}`;

    // Main Stream & Overlay
    if (data.cam_online) {
        elements.placeholderFocus.classList.add('hidden');
        elements.streamFocus.classList.remove('hidden');
        // MJPEG is more efficient than polling single frames for the main HUD view
        const streamUrl = `http://${window.location.hostname}:5000/api/frame/${state.selectedJct}?t=${Date.now()}`;
        if (!elements.streamFocus.src || Date.now() % 3000 < 500) elements.streamFocus.src = streamUrl;
    } else {
        elements.streamFocus.classList.add('hidden');
        elements.placeholderFocus.classList.remove('hidden');
        elements.streamFocus.src = '';
    }

    // AI Detections HUD
    let detHtml = '';
    if (data.ambulance) detHtml += '<span class="det" style="background:var(--ruby-glow);">🚑 AMBULANCE_DETECTED</span>';
    if (data.fire_truck) detHtml += '<span class="det" style="background:var(--gold-glow);">🚒 FIRE_TRUCK_DETECTED</span>';
    if (data.accident) detHtml += '<span class="det" style="background:#fff; color:#000;">⚠️ INCIDENT_ALERT</span>';
    elements.detsFocus.innerHTML = detHtml;

    // Emergency HUD Pulse
    const anyEm = Object.values(state.allJunctions).some(d => d.emergency || d.ambulance || d.fire_truck);
    if (anyEm) {
        document.body.classList.add('emergency-active');
    } else {
        document.body.classList.remove('emergency-active');
    }
}

/**
 * Command Protocol
 */
function sendCommand(target, action) {
    if (!state.connected) {
        addLogEntry(`>> ! SIMULATION_OVERRIDE: ${target} ← ${action}`);
        return;
    }

    if (target === 'SELECTED') {
        state.socket.emit('cmd', { junction: state.selectedJct, cmd: action });
        addLogEntry(`>> CMD_SENT: ${state.selectedJct} ← ${action}`);
    } else if (target === 'ALL') {
        state.junctions.forEach(j => {
            state.socket.emit('cmd', { junction: j, cmd: action });
        });
        addLogEntry(`>> GLOBAL_OVERRIDE_SENT: CITY ← ${action}`);
    } else if (target === 'GLOBAL' && action === 'CLEAR_EMERGENCY') {
        state.socket.emit('emergency_clear');
        addLogEntry('>> ✅ RESET: ALL EMERGENCY TRIGGERS CLEARED');
    }
}

// --- HUD Helpers ---
function updateNetworkList() {
    let listHtml = '';
    state.junctions.forEach(j => {
        const d = state.allJunctions[j] || {};
        listHtml += `
            <div class="term-line">
                <span class="ts">[${j}]</span> ${d._ip || '0.0.0.0'} -> 
                <span style="color:${d.cam_online ? 'var(--emerald-glow)' : 'var(--text-low)'}">${d.cam_online ? 'LINKED' : 'AWAITING'}</span>
            </div>
        `;
    });
    elements.networkList.innerHTML = listHtml;
}

function addLogEntry(msg) {
    const entry = document.createElement('div');
    entry.className = 'term-line';
    const ts = new Date().toLocaleTimeString([], { hour12: false });
    entry.innerHTML = `<span class="ts">[${ts}]</span> ${msg}`;
    
    elements.log.prepend(entry);
    if (elements.log.children.length > 50) elements.log.removeChild(elements.log.lastChild);
}

function getAlertSymbol(type) {
    switch(type) {
        case 'AMBULANCE': return '🚩';
        case 'FIRE_TRUCK': return '🚨';
        case 'ACCIDENT': return '⚠️';
        default: return '>>';
    }
}

function startSimulation() {
    setInterval(() => {
        if (state.connected) return;
        state.junctions.forEach(j => {
            state.allJunctions[j] = {
                vehicles: Math.floor(Math.random() * 5),
                traffic_score: Math.floor(Math.random() * 60),
                dist: Math.floor(40 + Math.random() * 100),
                signal: ['RED', 'GREEN', 'YELLOW'][Math.floor(Date.now() / 5000) % 3],
                cam_online: false
            };
        });
        updateUI();
    }, 2000);
}
