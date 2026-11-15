const renderers = new WeakMap();
const revisions = new WeakMap();

const vertexSource = `#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}`;

const fragmentSource = `#version 300 es
precision highp float;
precision highp int;
precision highp usampler2D;
uniform usampler2D planeY;
uniform usampler2D planeU;
uniform usampler2D planeV;
uniform ivec2 imageSize;
uniform ivec2 chromaSize;
uniform vec4 factors;
uniform vec4 offsets;
out vec4 result;

// MPEG2系の既定位置(left)で色差を補間する。整数テクスチャなので10bitも下位詰めのまま読める。
float chroma(usampler2D plane, ivec2 p) {
    vec2 q = (vec2(p) - vec2(0.0, 0.5)) * 0.5;
    ivec2 a = ivec2(floor(q));
    vec2 f = fract(q);
    ivec2 b = clamp(a + ivec2(1), ivec2(0), chromaSize - 1);
    a = clamp(a, ivec2(0), chromaSize - 1);
    float top = mix(float(texelFetch(plane, a, 0).r), float(texelFetch(plane, ivec2(b.x, a.y), 0).r), f.x);
    float bottom = mix(float(texelFetch(plane, ivec2(a.x, b.y), 0).r), float(texelFetch(plane, b, 0).r), f.x);
    return mix(top, bottom, f.y) - offsets.y;
}

void main() {
    ivec2 p = ivec2(int(gl_FragCoord.x), imageSize.y - 1 - int(gl_FragCoord.y));
    float y = (float(texelFetch(planeY, p, 0).r) - offsets.x) * offsets.z;
    float cb = chroma(planeU, p);
    float cr = chroma(planeV, p);
    vec3 rgb = vec3(y + factors.x * cr, y - factors.z * cb - factors.w * cr, y + factors.y * cb);
    // Coreと同じく最後に四捨五入し、範囲外を飽和させる。
    result = vec4(floor(clamp(rgb, 0.0, 255.0) + 0.5) / 255.0, 1.0);
}`;

function makeShader(gl, type, source) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        const message = gl.getShaderInfoLog(shader);
        gl.deleteShader(shader);
        throw new Error(message);
    }
    return shader;
}

function renderer(canvas) {
    if (renderers.has(canvas)) return renderers.get(canvas);
    // 表示用canvasは2Dに固定し、WebGL喪失後も同じ要素へPNGを表示できるようにする。
    const surface = document.createElement("canvas");
    const gl = surface.getContext("webgl2", { alpha: false, antialias: false, depth: false });
    const state = { surface, gl, lost: !gl, callback: null, textures: [], program: null };
    renderers.set(canvas, state);
    if (!gl) return state;
    surface.addEventListener("webglcontextlost", event => {
        event.preventDefault();
        state.lost = true;
        invalidate(canvas);
        if (state.callback) state.callback.invokeMethodAsync("OnPreviewContextLost").catch(() => {});
    });
    try {
        const vertex = makeShader(gl, gl.VERTEX_SHADER, vertexSource);
        const fragment = makeShader(gl, gl.FRAGMENT_SHADER, fragmentSource);
        const program = gl.createProgram();
        gl.attachShader(program, vertex);
        gl.attachShader(program, fragment);
        gl.linkProgram(program);
        gl.deleteShader(vertex);
        gl.deleteShader(fragment);
        state.program = program;
        if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
        state.textures = [gl.createTexture(), gl.createTexture(), gl.createTexture()];
    } catch {
        state.lost = true;
    }
    return state;
}

function normalize(value) { return String(value ?? "").trim().toLowerCase(); }

function colorParameters(metadata, depth, width, height) {
    let kr, kb;
    switch (normalize(metadata.matrix)) {
        case "bt709": case "709": kr = .2126; kb = .0722; break;
        case "bt601": case "601": case "smpte170m": case "bt470bg": kr = .299; kb = .114; break;
        case "bt2020nc": case "bt2020ncl": case "bt2020-ncl": kr = .2627; kb = .0593; break;
        case "smpte240m": case "smpte240": kr = .212; kb = .087; break;
        default: [kr, kb] = width > 1024 || height > 576 ? [.2126, .0722] : [.299, .114]; break;
    }
    const full = ["full", "pc", "jpeg"].includes(normalize(metadata.range));
    const scale = 2 ** (depth - 8), kg = 1 - kr - kb;
    const yFactor = 255 / (full ? 2 ** depth - 1 : 219 * scale);
    const cFactor = 255 / (full ? 2 ** depth - 1 : 224 * scale);
    return {
        factors: [2 * (1 - kr) * cFactor, 2 * (1 - kb) * cFactor,
            2 * kb * (1 - kb) / kg * cFactor, 2 * kr * (1 - kr) / kg * cFactor],
        offsets: [full ? 0 : 16 * scale, 128 * scale, yFactor, 0]
    };
}

function drawYuv(canvas, bytes, metadata) {
    const state = renderer(canvas), gl = state.gl;
    if (state.lost || !gl || gl.isContextLost()) return false;
    // HDRはCoreのトーンマップを使うPNGへ退避する。
    if (["smpte2084", "pq", "arib-std-b67", "hlg"].includes(normalize(metadata.transfer))) return false;
    const format = normalize(metadata.format);
    const depth = format === "yuv420p10" ? 10 : format === "yuv420p8" ? 8 : 0;
    const width = Number(metadata.width), height = Number(metadata.height);
    if (!depth || !Number.isSafeInteger(width) || !Number.isSafeInteger(height) || width <= 0 || height <= 0) return false;
    const sampleBytes = depth > 8 ? 2 : 1, cw = Math.ceil(width / 2), ch = Math.ceil(height / 2);
    const pitches = String(metadata.pitches ?? "").split(",").map(Number);
    const widths = [width, cw, cw], heights = [height, ch, ch];
    if (pitches.length !== 3 || pitches.some((p, i) => !Number.isSafeInteger(p) || p < widths[i] * sampleBytes || p % sampleBytes)) return false;
    const expected = pitches.reduce((total, p, i) => total + p * heights[i], 0);
    if (bytes.length !== expected || width > gl.getParameter(gl.MAX_TEXTURE_SIZE) || height > gl.getParameter(gl.MAX_TEXTURE_SIZE)) return false;
    if (state.surface.width !== width) state.surface.width = width;
    if (state.surface.height !== height) state.surface.height = height;
    gl.viewport(0, 0, width, height);
    gl.disable(gl.DITHER);
    gl.useProgram(state.program);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    let offset = 0;
    for (let i = 0; i < 3; ++i) {
        const length = pitches[i] * heights[i];
        const plane = bytes.subarray(offset, offset + length);
        // Uint16Arrayのアライメントとホストのエンディアンに依存しない。
        let samples = plane;
        if (sampleBytes === 2) {
            samples = new Uint16Array(length / 2);
            for (let j = 0; j < samples.length; ++j) samples[j] = plane[j * 2] | plane[j * 2 + 1] << 8;
        }
        gl.activeTexture(gl.TEXTURE0 + i);
        gl.bindTexture(gl.TEXTURE_2D, state.textures[i]);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        gl.pixelStorei(gl.UNPACK_ROW_LENGTH, pitches[i] / sampleBytes);
        gl.texImage2D(gl.TEXTURE_2D, 0, sampleBytes === 2 ? gl.R16UI : gl.R8UI, widths[i], heights[i], 0,
            gl.RED_INTEGER, sampleBytes === 2 ? gl.UNSIGNED_SHORT : gl.UNSIGNED_BYTE, samples);
        gl.uniform1i(gl.getUniformLocation(state.program, ["planeY", "planeU", "planeV"][i]), i);
        offset += length;
    }
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
    const color = colorParameters(metadata, depth, width, height);
    gl.uniform2i(gl.getUniformLocation(state.program, "imageSize"), width, height);
    gl.uniform2i(gl.getUniformLocation(state.program, "chromaSize"), cw, ch);
    gl.uniform4fv(gl.getUniformLocation(state.program, "factors"), color.factors);
    gl.uniform4fv(gl.getUniformLocation(state.program, "offsets"), color.offsets);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    if (gl.getError() !== gl.NO_ERROR || gl.isContextLost()) return false;
    if (canvas.width !== width) canvas.width = width;
    if (canvas.height !== height) canvas.height = height;
    const context = canvas.getContext("2d");
    if (!context) return false;
    context.drawImage(state.surface, 0, 0);
    return true;
}

export async function draw(canvas, bytes, metadata, mime = "") {
    const revision = invalidate(canvas);
    try {
        if (!(bytes instanceof Uint8Array)) bytes = new Uint8Array(bytes);
        if (normalize(metadata.format).startsWith("yuv")) return drawYuv(canvas, bytes, metadata);
        const type = mime || (normalize(metadata.format) === "jpeg" ? "image/jpeg" : "image/png");
        const bitmap = await createImageBitmap(new Blob([bytes], { type }));
        try {
            if (revisions.get(canvas) !== revision) return false;
            const context = canvas.getContext("2d");
            if (!context) return false;
            if (canvas.width !== bitmap.width) canvas.width = bitmap.width;
            if (canvas.height !== bitmap.height) canvas.height = bitmap.height;
            context.drawImage(bitmap, 0, 0);
            return true;
        } finally { bitmap.close(); }
    } catch {
        return false;
    }
}

export function invalidate(canvas) {
    const revision = (revisions.get(canvas) ?? 0) + 1;
    revisions.set(canvas, revision);
    return revision;
}

export function contextLost(canvas) {
    const state = renderers.get(canvas);
    return !!state && (state.lost || !state.gl || state.gl.isContextLost());
}

export function setContextLostHandler(canvas, callback) {
    renderer(canvas).callback = callback;
}

export function dispose(canvas) {
    invalidate(canvas);
    const state = renderers.get(canvas);
    if (!state) return;
    state.callback = null;
    if (state.gl && !state.gl.isContextLost()) {
        for (const texture of state.textures) state.gl.deleteTexture(texture);
        if (state.program) state.gl.deleteProgram(state.program);
    }
    renderers.delete(canvas);
}

// 2枚を同じ呼出しで無効化し、awaitの間に新しい描画を巻き込まない。
export function invalidateAll(before, after) {
    invalidate(before);
    invalidate(after);
}

// DOMから外れた後も、作成時のcanvas参照でGPU資源を解放する。
export function bindCanvases(before, after, callback) {
    setContextLostHandler(before, callback);
    setContextLostHandler(after, callback);
    return { dispose() { dispose(before); dispose(after); } };
}
