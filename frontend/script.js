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
    junctions: ['J1', 'J2', 'J3', 'J4'],
    webcamActive: false,
    webcamStream: null,
    webcamInterval: null
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
    detsFocus: document.getElementById('dets-focus'),
    
    hiddenCanvas: document.getElementById('hidden-canvas'),
    insightsText: document.getElementById('insight-text'),
    
    // New Real-time Overlays
    localVideo: document.getElementById('local-video'),
    detCanvas: document.getElementById('det-canvas')
};

// High-fidelity SFX for Cinematic Feedback
const sfx = {
    beep: new Audio('https://www.soundjay.com/buttons/sounds/button-37.mp3'),
    alert: new Audio('https://www.soundjay.com/buttons/sounds/beep-01a.mp3'),
    emergency: new Audio('https://www.soundjay.com/buttons/sounds/beep-04.mp3')
};
sfx.beep.volume = 0.2;
sfx.alert.volume = 0.3;
sfx.emergency.volume = 0.4;

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

    state.socket.on('vision_update', (data) => {
        if (data.junction === state.selectedJct) {
            drawDetections(data.detections);
            // Low-latency stat update
            elements.svFocus.textContent = data.v || 0;
            elements.spFocus.textContent = data.p || 0;
            elements.tsFocus.textContent = `${data.t || 0}%`;
        }
    });
}

function drawDetections(detections) {
    const canvas = elements.detCanvas;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    
    // Ensure canvas internal resolution matches display size
    canvas.width = canvas.clientWidth;
    canvas.height = canvas.clientHeight;
    
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    
    detections.forEach(det => {
        const [x, y, w, h] = det.bbox;
        const cls = det.class.toUpperCase();
        
        // Scale normalized coords (0-640) to canvas size
        const scaleX = canvas.width / 640;
        const scaleY = canvas.height / 480;
        
        ctx.strokeStyle = '#06b6d4';
        ctx.lineWidth = 2;
        ctx.strokeRect(x * scaleX, y * scaleY, w * scaleX, h * scaleY);
        
        ctx.fillStyle = '#06b6d4';
        ctx.font = 'bold 10px JetBrains Mono';
        ctx.fillText(cls, x * scaleX, y * scaleY - 5);
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

    // AI Insight Dashboard Logic
    updateAIInsights(data);

    // Violation Strobe
    if (data.violation) {
        document.body.classList.add('violation-active');
    } else {
        document.body.classList.remove('violation-active');
    }

    // Emergency HUD Pulse
    const anyEm = Object.values(state.allJunctions).some(d => d.emergency || d.ambulance || d.fire_truck);
    if (anyEm) {
        document.body.classList.add('emergency-active');
        if (Math.random() > 0.96) sfx.emergency.play(); // Occasional warning chime
    } else {
        document.body.classList.remove('emergency-active');
    }
}

/**
 * AI Insight Engine
 */
function updateAIInsights(data) {
    if (!elements.insightsText) return;
    let advice = "// SYSTEM OPERATIONAL. FLOW STEADY.";
    
    if (data.traffic_score > 85) advice = `// LOAD CRITICAL!! Recommendation: Extend GREEN for ${state.selectedJct}.`;
    else if (data.ambulance || data.fire_truck) advice = `// EMERGENCY PRIORITY detected. Lane clearing active.`;
    else if (data.violation) advice = `// SECURITY BREACH!! Illegal movement at ${state.selectedJct}.`;
    else if (data.persons > 0) advice = `// PEDESTRIAN DETECTED. Holding signal for safety...`;
    else if (data.traffic_score < 15 && data.signal === 'RED') advice = `// LOW LOAD. Switching likely for efficiency.`;

    elements.insightsText.textContent = advice;
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
        sfx.beep.play();
    } else if (target === 'ALL') {
        state.junctions.forEach(j => {
            state.socket.emit('cmd', { junction: j, cmd: action });
        });
        addLogEntry(`>> GLOBAL_OVERRIDE_SENT: CITY ← ${action}`);
        sfx.alert.play();
    } else if (target === 'GLOBAL' && action === 'CLEAR_EMERGENCY') {
        state.socket.emit('emergency_clear');
        addLogEntry('>> ✅ RESET: ALL EMERGENCY TRIGGERS CLEARED');
        sfx.beep.play();
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

/**
 * Webcam Alternative Protocol
 */
async function toggleWebcam() {
    if (state.webcamActive) {
        // Stop Webcam
        if (state.webcamStream) {
            state.webcamStream.getTracks().forEach(track => track.stop());
        }
        clearInterval(state.webcamInterval);
        state.webcamActive = false;
        state.webcamStream = null;
        
        elements.localVideo.classList.add('hidden');
        elements.streamFocus.classList.remove('hidden');
        elements.webcamBtn.innerHTML = '<span id="webcam-status-dot" class="status-dot offline"></span> LOCAL WEBCAM: OFF';
        addLogEntry('>> ACCESS_TERMINATED: LOCAL_WEB_CAMERA_OFF');
        
        // Clear detections
        drawDetections([]);
    } else {
        // Start Webcam
        try {
            const constraints = { 
                video: { width: 640, height: 480, facingMode: "environment" } 
            };
            
            addLogEntry('>> REQUESTING_CAMERA_ACCESS...');
            state.webcamStream = await navigator.mediaDevices.getUserMedia(constraints);
            elements.localVideo.srcObject = state.webcamStream;
            state.webcamActive = true;
            
            elements.localVideo.classList.remove('hidden');
            elements.streamFocus.classList.add('hidden');
            elements.placeholderFocus.classList.add('hidden');

            elements.webcamBtn.innerHTML = '<span id="webcam-status-dot" class="status-dot online"></span> LOCAL WEBCAM: ACTIVE';
            addLogEntry('>> ✓ ACCESS_GRANTED: STREAMING LOCAL_FEED TO AI_CORE');

            // Real-time Push: 5 FPS (matches server INFER_FPS)
            state.webcamInterval = setInterval(captureAndPushFrame, 200);
        } catch (err) {
            addLogEntry(`>> ! ERROR: CAMERA_ACCESS_DENIED [${err.message}]`);
            console.error(err);
        }
    }
}

function captureAndPushFrame() {
    if (!state.connected || !state.webcamActive) return;

    const ctx = elements.hiddenCanvas.getContext('2d');
    // Draw from local-video instead of hidden video (though they are same stream)
    ctx.drawImage(elements.localVideo, 0, 0, 640, 480);
    const dataUrl = elements.hiddenCanvas.toDataURL('image/jpeg', 0.5); // Slightly lower quality for speed
    
    state.socket.emit('push_frame', {
        junction: state.selectedJct,
        image: dataUrl
    });
}
