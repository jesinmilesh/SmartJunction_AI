/**
 * SmartJunction AI - Frontend Logic
 * Author: Jesin Milesh
 */

// --- Configuration & State ---
const state = {
    connected: true,
    congestion: 0,
    distance: 999,
    vehicles: 0,
    pedestrians: 0,
    emergency: false,
    pedestrianZone: false
};

// --- DOM Elements ---
const elements = {
    congVal: document.getElementById('cong-val'),
    distVal: document.getElementById('dist-val'),
    vehCount: document.getElementById('veh-count'),
    pedCount: document.getElementById('ped-count'),
    congGauge: document.getElementById('congestion-gauge'),
    distGauge: document.getElementById('distance-gauge'),
    log: document.getElementById('detection-log'),
    status: document.getElementById('connection-status')
};

// --- Initialization ---
document.addEventListener('DOMContentLoaded', () => {
    addLogEntry('System Initialized.');
    addLogEntry('Awaiting MQTT data stream...');
    
    // Start simulation for demonstration purposes
    startSimulation();
});

/**
 * Updates the UI based on state changes
 */
function updateUI() {
    // Update numeric values
    elements.congVal.textContent = state.congestion;
    elements.distVal.textContent = state.distance === 999 ? '--' : state.distance;
    elements.vehCount.textContent = state.vehicles;
    elements.pedCount.textContent = state.pedestrians;

    // Update Congestion Gauge (0-100% -> 0-360deg)
    const congDegree = (state.congestion / 100) * 360;
    let color = '#06b6d4'; // default cyan
    if (state.congestion > 40) color = '#f59e0b'; // warning gold
    if (state.congestion > 70) color = '#ef4444'; // alert ruby
    
    elements.congGauge.style.background = `conic-gradient(${color} ${congDegree}deg, #1e293b 0deg)`;

    // Update Distance Gauge (Inverted logic: smaller distance = more progress)
    // Range 0-200cm
    const cappedDist = Math.max(0, Math.min(200, state.distance));
    const distPercent = 100 - (cappedDist / 200 * 100);
    const distDegree = (distPercent / 100) * 360;
    
    elements.distGauge.style.background = `conic-gradient(var(--accent-emerald) ${distDegree}deg, #1e293b 0deg)`;
}

/**
 * Sends a command to the backend (Simulated)
 * @param {string} cmd 
 */
function sendCommand(cmd) {
    const time = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    addLogEntry(`[COMMAND] Priority Override: ${cmd}`);
    
    // Simulate immediate UI reaction
    if (cmd === 'EMERGENCY') {
        state.emergency = true;
        document.body.style.boxShadow = 'inset 0 0 100px rgba(239, 68, 68, 0.2)';
    } else if (cmd === 'CLEAR') {
        state.emergency = false;
        document.body.style.boxShadow = 'none';
    }
}

/**
 * Adds an entry to the visual detection log
 * @param {string} msg 
 */
function addLogEntry(msg) {
    const entry = document.createElement('div');
    entry.className = 'log-entry';
    const time = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    entry.textContent = `[${time}] ${msg}`;
    
    elements.log.prepend(entry);
    
    // Keep log clean
    if (elements.log.children.length > 20) {
        elements.log.removeChild(elements.log.lastChild);
    }
}

/**
 * Simple data simulation for WOW factor during demo
 */
function startSimulation() {
    setInterval(() => {
        if (state.emergency) return;

        // Randomly fluctuate values
        state.congestion = Math.floor(Math.abs(Math.sin(Date.now() / 10000) * 100));
        state.distance = Math.floor(20 + Math.random() * 150);
        
        // Random detections
        const chance = Math.random();
        if (chance > 0.95) {
            state.vehicles = Math.floor(Math.random() * 5);
            if (state.vehicles > 0) addLogEntry(`${state.vehicles} vehicle(s) detected at North gate.`);
        }
        if (chance < 0.05) {
            state.pedestrians = Math.random() > 0.5 ? 1 : 0;
            if (state.pedestrians > 0) addLogEntry(`Pedestrian detected on East crosswalk.`);
        }

        updateUI();
    }, 1000);
}
