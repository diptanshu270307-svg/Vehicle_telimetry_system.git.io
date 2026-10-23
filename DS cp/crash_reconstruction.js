/**
 * Black-Box Crash Telemetry Reconstruction Engine
 * Handles multi-axis time-series visualization & frame-by-frame scrubber
 */

class CrashReconstructionEngine {
    constructor() {
        this.modal = document.getElementById('modal-reconstruct');
        this.chartCanvas = document.getElementById('reconstruct-chart');
        this.scrubber = document.getElementById('scrubber-slider');
        this.chart = null;
        this.currentIncident = null;
        this.currentPackets = [];

        this.initEvents();
    }

    initEvents() {
        if (this.scrubber) {
            this.scrubber.addEventListener('input', (e) => {
                this.updateFrame(parseInt(e.target.value));
            });
        }
    }

    close() {
        if (this.modal) this.modal.style.display = 'none';
    }

    async open(incidentId) {
        if (!this.modal) return;
        this.modal.style.display = 'flex';

        try {
            const resp = await fetch(`/api/reconstruct?id=${incidentId}`);
            const data = await resp.json();
            if (data.error) {
                alert(`Error: ${data.error}`);
                this.close();
                return;
            }

            this.currentIncident = data;
            this.currentPackets = data.packets || [];

            this.renderHeader(data);
            this.renderChart(this.currentPackets);

            if (this.scrubber) {
                this.scrubber.max = Math.max(0, this.currentPackets.length - 1);
                /* Find peak impact index as initial scrubber position */
                let peakIdx = 0;
                let maxG = 0;
                this.currentPackets.forEach((p, i) => {
                    if (p.g > maxG) {
                        maxG = p.g;
                        peakIdx = i;
                    }
                });
                this.scrubber.value = peakIdx;
                this.updateFrame(peakIdx);
            }
        } catch (err) {
            console.error('Failed to load crash reconstruction data:', err);
        }
    }

    renderHeader(data) {
        const titleElem = document.getElementById('recon-title');
        const descElem = document.getElementById('recon-desc');
        const hospElem = document.getElementById('recon-hospital');

        if (titleElem) {
            titleElem.innerHTML = `🚨 Crash Incident Reconstruction — <span style="color:#00f2fe;">NODE #${data.id}</span> (${data.type})`;
        }
        if (descElem) {
            descElem.textContent = `${data.description} | Peak Impact: ${data.peak_g.toFixed(2)}G | GPS: (${data.latitude.toFixed(5)}, ${data.longitude.toFixed(5)})`;
        }
        if (hospElem) {
            hospElem.innerHTML = `Nearest Dispatch: <strong style="color:#38bdf8;">${data.hospital}</strong> (${data.hospital_dist.toFixed(2)} km) | Hotline: <strong style="color:#10b981;">${data.hospital_phone}</strong>`;
        }
    }

    renderChart(packets) {
        if (!this.chartCanvas) return;

        const labels = packets.map((_, i) => `${i}`);
        const totalGData = packets.map(p => p.g);
        const axData = packets.map(p => p.ax);
        const ayData = packets.map(p => p.ay);
        const speedData = packets.map(p => p.speed);

        if (this.chart) {
            this.chart.destroy();
        }

        const ctx = this.chartCanvas.getContext('2d');
        this.chart = new Chart(ctx, {
            type: 'line',
            data: {
                labels: labels,
                datasets: [
                    {
                        label: 'Total G-Force Magnitude (G)',
                        data: totalGData,
                        borderColor: '#ef4444',
                        backgroundColor: 'rgba(239, 68, 68, 0.15)',
                        borderWidth: 2,
                        tension: 0.2,
                        fill: true,
                        yAxisID: 'yG'
                    },
                    {
                        label: 'Longitudinal Accel ax (G)',
                        data: axData,
                        borderColor: '#f59e0b',
                        borderWidth: 1.5,
                        tension: 0.2,
                        yAxisID: 'yG'
                    },
                    {
                        label: 'Lateral Accel ay (G)',
                        data: ayData,
                        borderColor: '#818cf8',
                        borderWidth: 1.5,
                        tension: 0.2,
                        yAxisID: 'yG'
                    },
                    {
                        label: 'Speed (km/h)',
                        data: speedData,
                        borderColor: '#00f2fe',
                        borderWidth: 2,
                        borderDash: [4, 4],
                        tension: 0.2,
                        yAxisID: 'ySpeed'
                    }
                ]
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                interaction: {
                    mode: 'index',
                    intersect: false
                },
                plugins: {
                    legend: {
                        labels: { color: '#94a3b8', font: { family: 'Inter', size: 11 } }
                    },
                    tooltip: {
                        backgroundColor: 'rgba(15, 23, 42, 0.95)',
                        titleColor: '#00f2fe',
                        borderColor: 'rgba(56, 189, 248, 0.3)',
                        borderWidth: 1
                    }
                },
                scales: {
                    x: {
                        grid: { color: 'rgba(255, 255, 255, 0.05)' },
                        ticks: { color: '#64748b', font: { family: 'JetBrains Mono', size: 10 } },
                        title: { display: true, text: 'Circular Buffer Packet Index (Pre-Crash Window -> Impact -> Post-Crash)', color: '#94a3b8' }
                    },
                    yG: {
                        type: 'linear',
                        display: true,
                        position: 'left',
                        grid: { color: 'rgba(255, 255, 255, 0.05)' },
                        ticks: { color: '#ef4444', font: { family: 'JetBrains Mono' } },
                        title: { display: true, text: 'G-Force', color: '#ef4444' }
                    },
                    ySpeed: {
                        type: 'linear',
                        display: true,
                        position: 'right',
                        grid: { drawOnChartArea: false },
                        ticks: { color: '#00f2fe', font: { family: 'JetBrains Mono' } },
                        title: { display: true, text: 'Speed (km/h)', color: '#00f2fe' }
                    }
                }
            }
        });
    }

    updateFrame(index) {
        if (!this.currentPackets || index < 0 || index >= this.currentPackets.length) return;
        const p = this.currentPackets[index];

        const setVal = (id, val) => {
            const el = document.getElementById(id);
            if (el) el.textContent = val;
        };

        setVal('snap-idx', `#${index}`);
        setVal('snap-speed', `${p.speed.toFixed(1)} km/h`);
        setVal('snap-rpm', `${Math.round(p.rpm)}`);
        setVal('snap-g', `${p.g.toFixed(2)} G`);
        setVal('snap-ax', `${p.ax.toFixed(2)} G`);
        setVal('snap-ay', `${p.ay.toFixed(2)} G`);
        setVal('snap-roll', `${p.roll.toFixed(1)}°`);
        setVal('snap-pitch', `${p.pitch.toFixed(1)}°`);
        setVal('snap-temp', `${p.temp.toFixed(1)} °C`);
        setVal('snap-score', `${(p.score * 100).toFixed(1)}%`);

        const timeDiffSec = ((index - (this.currentPackets.length - 1)) * 0.05).toFixed(2);
        const scrubberTime = document.getElementById('scrubber-time-offset');
        if (scrubberTime) {
            scrubberTime.textContent = `T = ${timeDiffSec}s relative to buffer end (Frame #${index})`;
        }
    }

    exportJSON() {
        if (!this.currentIncident) return;
        const str = JSON.stringify(this.currentIncident, null, 2);
        const blob = new Blob([str], { type: 'application/json' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `blackbox_incident_${this.currentIncident.id}.json`;
        a.click();
        URL.revokeObjectURL(url);
    }

    printReport() {
        window.print();
    }
}

window.CrashReconstruction = new CrashReconstructionEngine();
