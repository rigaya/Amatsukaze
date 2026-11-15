let sessionUrl;
function closeSession() {
    if (sessionUrl) fetch(sessionUrl, { method: "DELETE", keepalive: true }).catch(() => {});
}
export function attach(url) {
    detach();
    sessionUrl = url;
    window.addEventListener("pagehide", closeSession);
}
export function detach() {
    window.removeEventListener("pagehide", closeSession);
    sessionUrl = undefined;
}

export function inputFocused() {
    const element = document.activeElement;
    return !!element && (element.matches("input, select, textarea, [contenteditable=true]") || element.isContentEditable);
}
