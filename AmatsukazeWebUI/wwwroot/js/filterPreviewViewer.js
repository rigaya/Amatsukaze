const minimumZoom = 0.25;
const maximumZoom = 32;
const wheelZoom = 1.25;
const pixelsPerWheelStep = 100;
const splitGrip = 6;
const pixelTolerance = 1e-6;

export function bind(viewport, layer, before, after, divider, fitButton, actualSizeButton, zoomLabel, callback) {
    const listeners = new AbortController();
    const signal = listeners.signal;
    let width = 0, height = 0;
    let zoom = null;
    let split = 0.5, view = "after";
    let wheel = 0, stepping = false;
    let drag = null;

    function geometry() {
        const box = viewport.getBoundingClientRect();
        const dpr = window.devicePixelRatio || 1;
        const fit = Math.min(box.width / width, box.height / height);
        const scale = zoom ? zoom.scale / dpr : fit;
        return {
            box, dpr, fit, scale,
            x: box.width / 2 - (zoom?.x ?? width / 2) * scale,
            y: box.height / 2 - (zoom?.y ?? height / 2) * scale
        };
    }

    function isIntegerScale(value) {
        return value >= 1 - pixelTolerance && Math.abs(value - Math.round(value)) <= pixelTolerance;
    }

    function useNearest(canvas, physicalScale) {
        return zoom && isIntegerScale(width * physicalScale / canvas.width)
            && isIntegerScale(height * physicalScale / canvas.height);
    }

    function render() {
        if (!width || !height) return;
        const { x, y, scale, dpr } = geometry();
        const physicalScale = scale * dpr;
        const beforeNearest = view !== "after" && useNearest(before, physicalScale);
        const afterNearest = view !== "before" && useNearest(after, physicalScale);
        // 補間なしで描くときは、前後の共通原点を物理画素にそろえる。
        const align = beforeNearest || afterNearest;
        const left = align ? Math.round(x * dpr) / dpr : x;
        const top = align ? Math.round(y * dpr) / dpr : y;
        layer.style.transform = `translate(${left}px, ${top}px) scale(${scale})`;
        before.style.imageRendering = beforeNearest ? "pixelated" : "auto";
        after.style.imageRendering = afterNearest ? "pixelated" : "auto";
        before.style.clipPath = view === "compare" ? `inset(0 ${(1 - split) * 100}% 0 0)` : "";
        divider.style.left = `${left + width * scale * split}px`;
        zoomLabel.value = `${Math.round(physicalScale * 100)}%`;
    }

    function zoomAt(clientX, clientY, factor) {
        const { box, x, y, scale, fit, dpr } = geometry();
        const next = Math.max(Math.min(minimumZoom, fit * dpr), Math.min(maximumZoom, scale * dpr * factor));
        const px = clientX - box.left, py = clientY - box.top;
        zoom = {
            scale: next,
            x: (px - x) / scale + (box.width / 2 - px) * dpr / next,
            y: (py - y) / scale + (box.height / 2 - py) * dpr / next
        };
        render();
    }

    function fit() {
        zoom = null;
        render();
    }

    function actualSize() {
        if (!width || !height) return;
        zoom = { scale: 1, x: zoom?.x ?? width / 2, y: zoom?.y ?? height / 2 };
        render();
    }

    async function step(direction) {
        stepping = true;
        try {
            await callback.invokeMethodAsync("StepPreviewAsync", direction);
        } finally {
            stepping = false;
        }
    }

    function onWheel(event) {
        event.preventDefault();
        if (!width || !height || !event.deltaY) return;
        const pixels = event.deltaMode === WheelEvent.DOM_DELTA_PIXEL ? event.deltaY : event.deltaY * pixelsPerWheelStep;
        if (event.ctrlKey) {
            zoomAt(event.clientX, event.clientY, wheelZoom ** (-pixels / pixelsPerWheelStep));
            return;
        }
        if (stepping) return;
        wheel += pixels;
        const count = Math.trunc(wheel / pixelsPerWheelStep);
        wheel -= count * pixelsPerWheelStep;
        if (count) step(Math.sign(count)).catch(error => { if (!signal.aborted) console.error(error); });
    }

    function isOnDivider(event) {
        if (view !== "compare") return false;
        const left = divider.getBoundingClientRect().left;
        return Math.abs(event.clientX - left) <= splitGrip;
    }

    function updateCursor(event) {
        viewport.style.cursor = (drag ? drag.split : isOnDivider(event)) ? "ew-resize" : drag ? "grabbing" : "grab";
    }

    function onPointerDown(event) {
        if (event.button !== 0 || !width || !height || drag || event.target.closest(".fa-overlay")) return;
        viewport.focus({ preventScroll: true });
        drag = { id: event.pointerId, x: event.clientX, y: event.clientY, split: isOnDivider(event) };
        viewport.setPointerCapture(event.pointerId);
        updateCursor(event);
    }

    function onPointerMove(event) {
        if (!width || !height) return;
        if (drag?.id === event.pointerId) {
            const { box, x, scale, dpr } = geometry();
            if (drag.split) {
                split = Math.max(0, Math.min(1, (event.clientX - box.left - x) / (width * scale)));
            } else {
                zoom = {
                    scale: scale * dpr,
                    x: (zoom?.x ?? width / 2) - (event.clientX - drag.x) / scale,
                    y: (zoom?.y ?? height / 2) - (event.clientY - drag.y) / scale
                };
            }
            drag.x = event.clientX;
            drag.y = event.clientY;
            render();
        }
        updateCursor(event);
    }

    function endDrag(event) {
        if (drag?.id !== event.pointerId) return;
        drag = null;
        updateCursor(event);
    }

    const resize = new ResizeObserver(render);
    resize.observe(viewport);
    window.addEventListener("resize", render, { signal });
    viewport.addEventListener("wheel", onWheel, { passive: false, signal });
    viewport.addEventListener("pointerdown", onPointerDown, { signal });
    viewport.addEventListener("pointermove", onPointerMove, { signal });
    viewport.addEventListener("lostpointercapture", endDrag, { signal });
    viewport.addEventListener("dblclick", event => {
        if (!event.target.closest(".fa-overlay")) fit();
    }, { signal });
    fitButton.addEventListener("click", fit, { signal });
    actualSizeButton.addEventListener("click", actualSize, { signal });

    return {
        present(nextView, nextWidth, nextHeight) {
            view = nextView;
            // 解像度変更後も中央に表示する画像上の相対位置を保つ。
            if (zoom && width && height) {
                zoom.x *= nextWidth / width;
                zoom.y *= nextHeight / height;
            }
            width = nextWidth;
            height = nextHeight;
            layer.style.width = `${width}px`;
            layer.style.height = `${height}px`;
            render();
        },
        dispose() {
            resize.disconnect();
            listeners.abort();
        }
    };
}
