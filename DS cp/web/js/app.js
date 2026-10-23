/**
 * Main Application Controller
 * Handles SSE streams, Leaflet map, gauges, and simulation actions
 */

class App {
    constructor() {
        this.gauges = new GaugeRenderer();
        this.dsVis = new DataStructuresVisualizer();
        this.map = null;
        this.vehicleMarker = null;
        this.routePolyline = null;
        this.emergencyRouteLine = null;
        this.stationMarkers = [];
        this.pathCoords = [];
        this.lastIncidentId = 0;
        this.isSirenEnabled = true;

        this.initMap();
        this.loadStations();
        this.initSSE();
        this.initControls();
        this.startPollers();
    }

    initMap() {
        if (!document.getElementById('gps-map')) return;

        /* Initialize Leaflet map */
        this.map = L.map('gps-map', {
            zoomControl: false,
            attributionControl: false
        }).setView([18.5204, 73.8567], 14);

        L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png', {
            maxZoom: 19
        }).addTo(this.map);

        /* Vehicle Marker */
        const carIcon = L.divIcon({
            className: 'custom-car-icon',
            html: '<div style="font-size:24px; filter: drop-shadow(0 0 8px #00f2fe);">🚗</div>',
            iconSize: [28, 28],
            iconAnchor: [14, 14]
        });

        this.vehicleMarker = L.marker([18.5204, 73.8567], { icon: carIcon }).addTo(this.map);

        /* Breadcrumb Trail */
        this.routePolyline = L.polyline([], {
            color: '#00f2fe',
            weight: 3,
            opacity: 0.8,
            dashArray: '4, 4'
        }).addTo(this.map);
    }

    async loadStations() {
        try {
            const resp = await fetch('/api/stations');
            const stations = await resp.json();

            stations.forEach(st => {
                const hospIcon = L.divIcon({
                    className: 'hospital-icon',
                    html: '<div style="font-size:20px; filter: drop-shadow(0 0 6px #ef4444);">🏥</div>',
                    iconSize: [24, 24],
                    iconAnchor: [12, 12]
                });

                const m = L.marker([st.lat, st.lon], { icon: hospIcon }).addTo(this.map);
                m.bindPopup(`
                    <div style="color: #0f172a; font-family: sans-serif;">
                        <strong style="color: #dc2626;">${st.name}</strong><br>
                        <span style="font-size: 11px;">${st.type}</span><br>
                        <span style="font-size: 11px; font-weight: bold;">📞 ${st.phone}</span>
                    </div>
                `);
                this.stationMarkers.push({ data: st, marker: m });
            });
        } catch (e) {
            console.warn('Could not load emergency facilities:', e);
        }
    }

    initSSE() {
        const streamStatusElem = document.getElementById('status-stream');
        const evtSource = new EventSource('/api/events');

        evtSource.onopen = () => {
            if (streamStatusElem) {
                streamStatusElem.innerHTML = '<span class="badge-pulse"></span> LIVE (SSE @ 20Hz)';
                streamStatusElem.className = 'badge badge-live';
            }
        };

        evtSource.onmessage = (e) => {
            try {
                const msg = JSON.parse(e.data);
                if (msg.event === 'telemetry') {
                    this.handleTelemetry(msg);
                } else if (msg.event === 'incident') {
                    this.handleEmergencyIncident(msg);
                }
            } catch (err) {
                console.error('Error parsing SSE message:', err);
            }
        };

        evtSource.onerror = () => {
            if (streamStatusElem) {
                streamStatusElem.textContent = 'RECONNECTING...';
                streamStatusElem.className = 'badge badge-latency';
            }
        };
    }

    handleTelemetry(data) {
        /* Update KPIs */
        this.setVal('kpi-speed', Math.round(data.speed));
        this.setVal('kpi-g', data.g.toFixed(2));
        this.setVal('kpi-temp', Math.round(data.temp));
        this.setVal('kpi-score', `${(data.score * 100).toFixed(0)}%`);

        /* Update Gauges */
        this.gauges.drawSpeed(data.speed);
        this.gauges.drawRPM(data.rpm);
        this.gauges.drawGForce(data.ax, data.ay, data.g);

        /* Update Attitude readouts */
        this.setVal('attitude-roll', `${data.roll.toFixed(1)}°`);
        this.setVal('attitude-pitch', `${data.pitch.toFixed(1)}°`);
        this.setVal('attitude-yaw', `${Math.round(data.yaw)}°`);

        /* Update Edge Performance Badge */
        const latElem = document.getElementById('status-latency');
        if (latElem && data.state) {
            latElem.textContent = `EDGE LATENCY: ${data.state.lat_us.toFixed(2)} µs`;
        }

        const pktsElem = document.getElementById('kpi-total-pkts');
        if (pktsElem && data.state) {
            pktsElem.textContent = `${data.state.total_pkts.toLocaleString()} Packets Processed`;
        }

        /* Update Leaflet GPS Map */
        if (this.map && this.vehicleMarker && data.lat && data.lon) {
            const newPos = [data.lat, data.lon];
            this.vehicleMarker.setLatLng(newPos);

            this.pathCoords.push(newPos);
            if (this.pathCoords.length > 150) this.pathCoords.shift();
            this.routePolyline.setLatLngs(this.pathCoords);

            /* Auto-pan smoothly if vehicle drifts off-center */
            if (!this.map.getBounds().contains(newPos)) {
                this.map.panTo(newPos);
            }
        }

        /* Update Circular Queue Visualizer */
        if (data.queue) {
            this.dsVis.renderCircularQueue(data.queue, data);
        }
    }

    handleEmergencyIncident(data) {
        /* Show Emergency Banner */
        const banner = document.getElementById('emergency-banner');
        if (banner) {
            banner.style.display = 'block';
            document.getElementById('emergency-title').textContent = `CRITICAL ALERT: ${data.type}`;
            document.getElementById('emergency-detail').textContent =
                `${data.desc} | Peak G: ${data.peak_g.toFixed(2)}G | Instant Localization: Nearest Facility: ${data.hospital} (${data.hospital_dist.toFixed(2)} km)`;
        }

        /* Update Nearest Facility Card */
        this.setVal('hud-hospital-name', data.hospital);
        this.setVal('hud-hospital-dist', `${data.hospital_dist.toFixed(2)} km`);
        this.setVal('hud-hospital-phone', data.phone);

        /* Draw Emergency Dispatch Route Line */
        if (this.map && data.lat && data.lon) {
            /* Find target hospital coordinates from loaded stations */
            const targetStation = this.stationMarkers.find(s => s.data.name === data.hospital);
            if (targetStation) {
                if (this.emergencyRouteLine) this.map.removeLayer(this.emergencyRouteLine);
                this.emergencyRouteLine = L.polyline([
                    [data.lat, data.lon],
                    [targetStation.data.lat, targetStation.data.lon]
                ], {
                    color: '#ef4444',
                    weight: 4,
                    dashArray: '8, 8',
                    opacity: 0.9
                }).addTo(this.map);
            }
        }

        /* Beep siren alert using Web Audio API */
        if (this.isSirenEnabled) {
            this.playAlertSound();
        }

        /* Refresh incidents */
        this.fetchIncidents();
    }

    playAlertSound() {
        try {
            const ctx = new (window.AudioContext || window.webkitAudioContext)();
            const osc = ctx.createOscillator();
            const gain = ctx.createGain();
            osc.type = 'sawtooth';
            osc.frequency.setValueAtTime(880, ctx.currentTime);
            osc.frequency.exponentialRampToValueAtTime(440, ctx.currentTime + 0.3);
            gain.gain.setValueAtTime(0.3, ctx.currentTime);
            gain.gain.exponentialRampToValueAtTime(0.01, ctx.currentTime + 0.3);
            osc.connect(gain);
            gain.connect(ctx.destination);
            osc.start();
            osc.stop(ctx.currentTime + 0.3);
        } catch (e) {
            /* Audio not allowed without interaction */
        }
    }

    async fetchIncidents() {
        try {
            const resp = await fetch('/api/incidents');
            const incidents = await resp.json();
            this.dsVis.renderLinkedList(incidents);
        } catch (e) {
            console.error('Failed to fetch incidents:', e);
        }
    }

    startPollers() {
        this.fetchIncidents();
        setInterval(() => this.fetchIncidents(), 2000);
    }

    initControls() {
        /* Simulation Scenarios */
        window.triggerScenario = async (scenario) => {
            try {
                await fetch(`/api/simulate?scenario=${scenario}`, { method: 'POST' });
            } catch (e) {
                console.error('Simulation trigger failed:', e);
            }
        };

        window.resetSystem = async () => {
            try {
                await fetch('/api/reset', { method: 'POST' });
                const banner = document.getElementById('emergency-banner');
                if (banner) banner.style.display = 'none';
                if (this.emergencyRouteLine && this.map) {
                    this.map.removeLayer(this.emergencyRouteLine);
                    this.emergencyRouteLine = null;
                }
                this.pathCoords = [];
                if (this.routePolyline) this.routePolyline.setLatLngs([]);
                this.fetchIncidents();
            } catch (e) {
                console.error('Reset failed:', e);
            }
        };

        window.toggleSiren = () => {
            this.isSirenEnabled = !this.isSirenEnabled;
            const btn = document.getElementById('btn-siren-toggle');
            if (btn) {
                btn.textContent = this.isSirenEnabled ? '🔊 Siren: ON' : '🔇 Siren: OFF';
            }
        };
    }

    setVal(id, val) {
        const el = document.getElementById(id);
        if (el) el.textContent = val;
    }
}

document.addEventListener('DOMContentLoaded', () => {
    window.App = new App();
});
