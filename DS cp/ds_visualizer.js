/**
 * Data Structures Visualizers:
 * 1. Circular Queue (Ring Buffer)
 * 2. Incident Doubly-Linked List
 */

class DataStructuresVisualizer {
    constructor() {
        this.cqCanvas = document.getElementById('cq-canvas');
        this.cqCtx = this.cqCanvas ? this.cqCanvas.getContext('2d') : null;
        this.llContainer = document.getElementById('ll-container');

        this.cqHeadVal = document.getElementById('cq-head-val');
        this.cqTailVal = document.getElementById('cq-tail-val');
        this.cqCountVal = document.getElementById('cq-count-val');
        this.cqStatusText = document.getElementById('cq-status-text');
        this.llCountBadge = document.getElementById('ll-count-badge');

        this.numVisualSegments = 32; /* Scaled visual representation of buffer */
        this.renderedIncidentIds = new Set();
    }

    renderCircularQueue(queueStats, currentPacket) {
        if (!this.cqCtx || !queueStats) return;

        const ctx = this.cqCtx;
        const w = this.cqCanvas.width;
        const h = this.cqCanvas.height;
        const cx = w / 2;
        const cy = h / 2;
        const rOuter = w * 0.44;
        const rInner = w * 0.28;

        ctx.clearRect(0, 0, w, h);

        const { head, tail, count, capacity, frozen } = queueStats;

        /* Map 100-capacity head/tail to visual segments */
        const headSeg = Math.floor((head / capacity) * this.numVisualSegments) % this.numVisualSegments;
        const tailSeg = Math.floor((tail / capacity) * this.numVisualSegments) % this.numVisualSegments;

        const segAngle = (2 * Math.PI) / this.numVisualSegments;
        const padAngle = 0.03;

        /* Draw Segments */
        for (let i = 0; i < this.numVisualSegments; i++) {
            const startA = i * segAngle - Math.PI / 2;
            const endA = startA + segAngle - padAngle;

            ctx.beginPath();
            ctx.arc(cx, cy, rOuter, startA, endA);
            ctx.arc(cx, cy, rInner, endA, startA, true);
            ctx.closePath();

            /* Determine color */
            let isFilled = false;
            if (count === capacity) {
                isFilled = true;
            } else if (count > 0) {
                const normHead = headSeg;
                const normTail = tailSeg;
                if (normHead >= normTail) {
                    isFilled = (i >= normTail && i < normHead);
                } else {
                    isFilled = (i >= normTail || i < normHead);
                }
            }

            if (frozen) {
                ctx.fillStyle = (i === headSeg) ? 'rgba(239, 68, 68, 0.9)' : 'rgba(239, 68, 68, 0.35)';
            } else if (!isFilled) {
                ctx.fillStyle = 'rgba(255, 255, 255, 0.05)';
            } else if (i === headSeg) {
                ctx.fillStyle = '#00f2fe'; /* Active Write Head */
            } else if (i === tailSeg) {
                ctx.fillStyle = '#f59e0b'; /* Read Tail */
            } else {
                ctx.fillStyle = 'rgba(56, 189, 248, 0.3)';
            }

            ctx.fill();
        }

        /* Draw Pointer Indicators */
        const drawPointer = (segIdx, color, label) => {
            const angle = segIdx * segAngle + (segAngle / 2) - Math.PI / 2;
            const px = cx + (rOuter + 14) * Math.cos(angle);
            const py = cy + (rOuter + 14) * Math.sin(angle);

            ctx.beginPath();
            ctx.arc(px, py, 4, 0, 2 * Math.PI);
            ctx.fillStyle = color;
            ctx.shadowColor = color;
            ctx.shadowBlur = 8;
            ctx.fill();
            ctx.shadowBlur = 0;
        };

        drawPointer(headSeg, '#00f2fe', 'HEAD');
        drawPointer(tailSeg, '#f59e0b', 'TAIL');

        /* Update DOM Info */
        if (this.cqHeadVal) this.cqHeadVal.textContent = head;
        if (this.cqTailVal) this.cqTailVal.textContent = tail;
        if (this.cqCountVal) this.cqCountVal.textContent = `${count} / ${capacity}`;

        const centerCountElem = document.getElementById('cq-center-count');
        if (centerCountElem) centerCountElem.textContent = count;

        if (this.cqStatusText) {
            if (frozen) {
                this.cqStatusText.textContent = 'BUFFER FROZEN (LOCKED)';
                this.cqStatusText.style.color = '#ef4444';
            } else {
                this.cqStatusText.textContent = 'ACTIVE (CIRCULAR FIFO)';
                this.cqStatusText.style.color = '#10b981';
            }
        }
    }

    renderLinkedList(incidents) {
        if (!this.llContainer || !incidents) return;

        if (this.llCountBadge) {
            this.llCountBadge.textContent = `${incidents.length} Nodes`;
        }

        if (incidents.length === 0) {
            this.llContainer.innerHTML = '<div class="ll-empty">No anomalies recorded yet. Buffer is operating in rolling circular mode.</div>';
            this.renderedIncidentIds.clear();
            return;
        }

        /* Check if updated */
        const currentIds = new Set(incidents.map(inc => inc.id));
        let changed = false;
        if (currentIds.size !== this.renderedIncidentIds.size) {
            changed = true;
        } else {
            for (let id of currentIds) {
                if (!this.renderedIncidentIds.has(id)) {
                    changed = true;
                    break;
                }
            }
        }

        if (!changed) return;
        this.renderedIncidentIds = currentIds;

        /* Re-render nodes with memory pointers */
        let html = '';
        incidents.forEach((inc, idx) => {
            const baseAddr = 0x7fa800 + (inc.id * 0x120);
            const prevAddr = idx > 0 ? `0x${(baseAddr - 0x120).toString(16)}` : 'NULL (HEAD)';
            const nextAddr = idx < incidents.length - 1 ? `0x${(baseAddr + 0x120).toString(16)}` : 'NULL (TAIL)';
            const curAddr = `0x${baseAddr.toString(16)}`;

            const severityClass = inc.severity === 'CRITICAL' ? 'badge-danger' : 'badge-warning';

            html += `
            <div class="ll-node-card" id="node-${inc.id}">
                <div class="ll-node-header">
                    <span class="ll-node-id">NODE #${inc.id} (${curAddr})</span>
                    <span class="badge ${inc.severity === 'CRITICAL' ? 'btn-danger' : 'btn-warning'} btn-sm">${inc.severity}</span>
                </div>
                <div class="ll-node-body">
                    <div>
                        <strong style="color: #ffffff;">${inc.type}</strong>
                        <div style="font-size: 0.75rem; color: #94a3b8; margin-top: 2px;">
                            Impact: <strong style="color: #fca5a5;">${inc.peak_g.toFixed(2)}G</strong> | Speed: ${inc.impact_speed.toFixed(1)} km/h
                        </div>
                        <div style="font-size: 0.72rem; color: #38bdf8; margin-top: 2px;">
                            🚑 ${inc.hospital} (${inc.hospital_dist.toFixed(2)} km)
                        </div>
                    </div>
                </div>
                <div style="display: flex; justify-content: space-between; align-items: center; margin-top: 6px;">
                    <span class="ll-node-link-info">prev: ${prevAddr} | next: ${nextAddr}</span>
                    <button class="btn btn-primary btn-sm" onclick="window.CrashReconstruction.open(${inc.id})">
                        🔍 Reconstruct Black Box
                    </button>
                </div>
            </div>`;
        });

        this.llContainer.innerHTML = html;
    }
}
