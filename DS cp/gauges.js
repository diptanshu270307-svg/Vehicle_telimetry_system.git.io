/**
 * Automotive Canvas Gauges (Speedometer, Tachometer, G-Force Crosshair)
 */

class GaugeRenderer {
    constructor() {
        this.speedCanvas = document.getElementById('gauge-speed');
        this.rpmCanvas = document.getElementById('gauge-rpm');
        this.gCanvas = document.getElementById('gauge-g');

        this.speedCtx = this.speedCanvas ? this.speedCanvas.getContext('2d') : null;
        this.rpmCtx = this.rpmCanvas ? this.rpmCanvas.getContext('2d') : null;
        this.gCtx = this.gCanvas ? this.gCanvas.getContext('2d') : null;
    }

    drawSpeed(speed) {
        if (!this.speedCtx) return;
        const ctx = this.speedCtx;
        const w = this.speedCanvas.width;
        const h = this.speedCanvas.height;
        const cx = w / 2;
        const cy = h / 2;
        const r = w * 0.42;

        ctx.clearRect(0, 0, w, h);

        const startAngle = 0.75 * Math.PI;
        const endAngle = 2.25 * Math.PI;
        const totalAngle = endAngle - startAngle;

        /* Outer Track */
        ctx.beginPath();
        ctx.arc(cx, cy, r, startAngle, endAngle);
        ctx.lineWidth = 10;
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.08)';
        ctx.lineCap = 'round';
        ctx.stroke();

        /* Value Arc */
        const maxSpeed = 160.0;
        const clampedSpeed = Math.min(Math.max(speed, 0), maxSpeed);
        const valueAngle = startAngle + (clampedSpeed / maxSpeed) * totalAngle;

        ctx.beginPath();
        ctx.arc(cx, cy, r, startAngle, valueAngle);
        ctx.lineWidth = 10;
        const grad = ctx.createLinearGradient(0, 0, w, h);
        grad.addColorStop(0, '#00f2fe');
        grad.addColorStop(0.7, '#38bdf8');
        grad.addColorStop(1, '#ef4444');
        ctx.strokeStyle = grad;
        ctx.lineCap = 'round';
        ctx.stroke();

        /* Ticks */
        for (let i = 0; i <= 8; i++) {
            const tickAngle = startAngle + (i / 8) * totalAngle;
            const x1 = cx + (r - 12) * Math.cos(tickAngle);
            const y1 = cy + (r - 12) * Math.sin(tickAngle);
            const x2 = cx + (r - 18) * Math.cos(tickAngle);
            const y2 = cy + (r - 18) * Math.sin(tickAngle);

            ctx.beginPath();
            ctx.moveTo(x1, y1);
            ctx.lineTo(x2, y2);
            ctx.strokeStyle = 'rgba(255, 255, 255, 0.3)';
            ctx.lineWidth = 2;
            ctx.stroke();
        }

        /* Center Digital readout */
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillStyle = '#ffffff';
        ctx.font = 'bold 26px "JetBrains Mono", monospace';
        ctx.fillText(Math.round(speed), cx, cy - 2);

        ctx.fillStyle = '#94a3b8';
        ctx.font = '10px "Inter", sans-serif';
        ctx.fillText('KM/H', cx, cy + 18);
    }

    drawRPM(rpm) {
        if (!this.rpmCtx) return;
        const ctx = this.rpmCtx;
        const w = this.rpmCanvas.width;
        const h = this.rpmCanvas.height;
        const cx = w / 2;
        const cy = h / 2;
        const r = w * 0.42;

        ctx.clearRect(0, 0, w, h);

        const startAngle = 0.75 * Math.PI;
        const endAngle = 2.25 * Math.PI;
        const totalAngle = endAngle - startAngle;

        /* Outer Track */
        ctx.beginPath();
        ctx.arc(cx, cy, r, startAngle, endAngle);
        ctx.lineWidth = 10;
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.08)';
        ctx.lineCap = 'round';
        ctx.stroke();

        /* Value Arc */
        const maxRPM = 7000.0;
        const clampedRPM = Math.min(Math.max(rpm, 0), maxRPM);
        const valueAngle = startAngle + (clampedRPM / maxRPM) * totalAngle;

        ctx.beginPath();
        ctx.arc(cx, cy, r, startAngle, valueAngle);
        ctx.lineWidth = 10;
        const grad = ctx.createLinearGradient(0, 0, w, h);
        grad.addColorStop(0, '#38bdf8');
        grad.addColorStop(0.7, '#f59e0b');
        grad.addColorStop(1, '#ef4444');
        ctx.strokeStyle = grad;
        ctx.lineCap = 'round';
        ctx.stroke();

        /* Center Digital readout */
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillStyle = '#ffffff';
        ctx.font = 'bold 24px "JetBrains Mono", monospace';
        ctx.fillText(Math.round(rpm), cx, cy - 2);

        ctx.fillStyle = '#94a3b8';
        ctx.font = '10px "Inter", sans-serif';
        ctx.fillText('RPM', cx, cy + 18);
    }

    drawGForce(ax, ay, totalG) {
        if (!this.gCtx) return;
        const ctx = this.gCtx;
        const w = this.gCanvas.width;
        const h = this.gCanvas.height;
        const cx = w / 2;
        const cy = h / 2;
        const maxR = w * 0.42;

        ctx.clearRect(0, 0, w, h);

        /* Concentric G circles (1G, 2G, 4G limit) */
        const rings = [0.3, 0.6, 1.0];
        rings.forEach((scale, idx) => {
            ctx.beginPath();
            ctx.arc(cx, cy, maxR * scale, 0, 2 * Math.PI);
            ctx.lineWidth = 1;
            ctx.strokeStyle = idx === 2 ? 'rgba(239, 68, 68, 0.4)' : 'rgba(255, 255, 255, 0.1)';
            ctx.stroke();
        });

        /* Crosshairs */
        ctx.beginPath();
        ctx.moveTo(cx - maxR, cy);
        ctx.lineTo(cx + maxR, cy);
        ctx.moveTo(cx, cy - maxR);
        ctx.lineTo(cx, cy + maxR);
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.15)';
        ctx.lineWidth = 1;
        ctx.stroke();

        /* Map ax (longitudinal) and ay (lateral) to canvas coordinates */
        const maxPlotG = 4.0;
        const px = cx + (ay / maxPlotG) * maxR;
        const py = cy - (ax / maxPlotG) * maxR;

        /* Glowing Vector Ball */
        ctx.beginPath();
        ctx.arc(px, py, 7, 0, 2 * Math.PI);
        const ballColor = totalG > 4.0 ? '#ef4444' : (totalG > 2.0 ? '#f59e0b' : '#00f2fe');
        ctx.fillStyle = ballColor;
        ctx.shadowColor = ballColor;
        ctx.shadowBlur = 12;
        ctx.fill();
        ctx.shadowBlur = 0;

        /* Bottom Readout */
        ctx.textAlign = 'center';
        ctx.textBaseline = 'bottom';
        ctx.fillStyle = ballColor;
        ctx.font = 'bold 13px "JetBrains Mono", monospace';
        ctx.fillText(`${totalG.toFixed(2)} G`, cx, h - 2);
    }
}
