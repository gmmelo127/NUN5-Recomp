struct Blit {
    width: u32,
    height: u32,
    current: u32,
    previous: u32,
    blend: u32,
    out_width: u32,
    out_height: u32,
    pad: u32,
}

@group(0) @binding(0) var<storage, read> pixels: array<u32>;
@group(0) @binding(1) var<uniform> blit: Blit;
// Recomp settings overlay (RGBA8, straight alpha), drawn by the host CPU.
@group(0) @binding(2) var menu_texture: texture_2d<f32>;
@group(0) @binding(3) var menu_sampler: sampler;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
}

@vertex
fn vs(@builtin(vertex_index) i: u32) -> VertexOut {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    var o: VertexOut;
    o.position = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = vec2<f32>(p.x, 1.0 - p.y);
    return o;
}

fn texel(base: u32, x: i32, y: i32) -> vec4<f32> {
    let cx = u32(clamp(x, 0, i32(blit.width) - 1));
    let cy = u32(clamp(y, 0, i32(blit.height) - 1));
    return unpack4x8unorm(pixels[base + cy * blit.width + cx]);
}

// Per-axis filtering. Magnified axes use "sharp bilinear" (crisp pixels, no
// ringing); minified axes average the whole footprint (a single bilinear tap
// there produces moire/scanlines).
fn texel_linear(base: u32, p: vec2<f32>) -> vec4<f32> {
    let o = floor(p - vec2<f32>(0.5));
    let f = p - vec2<f32>(0.5) - o;
    let x = i32(o.x);
    let y = i32(o.y);
    let top = mix(texel(base, x, y), texel(base, x + 1, y), f.x);
    let bottom = mix(texel(base, x, y + 1), texel(base, x + 1, y + 1), f.x);
    return mix(top, bottom, f.y);
}

fn sharp_ok(fp: f32) -> bool {
    // Sharpen only on an exact integer magnification; a fractional factor maps
    // most samples onto the neighbouring texel and tears thin features apart.
    if (fp >= 1.0) {
        return false;
    }
    let r = 1.0 / fp;
    let s = round(r);
    return s >= 2.0 && abs(r - s) < 0.02;
}

fn tap_pos(t: f32, fp: f32, i: u32) -> f32 {
    if (fp <= 1.0) {
        if (!sharp_ok(fp)) {
            return t;
        }
        let s = round(1.0 / fp);
        let d = fract(t) - 0.5;
        let region = 0.5 - 0.5 / s;
        return floor(t) + (d - clamp(d, -region, region)) * s;
    }
    return t + ((f32(i) + 0.5) * 0.25 - 0.5) * fp;
}

// Bilinear over the *native* pixel grid, reading the supersampled buffer: each
// native pixel is a `scale` x `scale` block, so we take the block centre and
// blend between blocks. Samples the framebuffer the way the PS2 video output
// does when its 512 lines are enlarged, which is what the reference (PCSX2)
// shows -- sampling sub-texels of the upscaled buffer instead would keep every
// texel block hard and look blockier than the console.
fn texel_native(base: u32, p: vec2<f32>, scale: f32) -> vec4<f32> {
    let o = floor(p - vec2<f32>(0.5));
    let f = p - vec2<f32>(0.5) - o;
    let c = (scale - 1.0) * 0.5;
    let x0 = i32(o.x);
    let y0 = i32(o.y);
    let xa = i32(f32(x0) * scale + c);
    let xb = i32(f32(x0 + 1) * scale + c);
    let ya = i32(f32(y0) * scale + c);
    let yb = i32(f32(y0 + 1) * scale + c);
    let top = mix(texel(base, xa, ya), texel(base, xb, ya), f.x);
    let bottom = mix(texel(base, xa, yb), texel(base, xb, yb), f.x);
    return mix(top, bottom, f.y);
}

fn sample(base: u32, uv: vec2<f32>) -> vec4<f32> {
    let size = vec2<f32>(f32(blit.width), f32(blit.height));
    let scale = max(f32((blit.pad >> 8u) & 0xFFu), 1.0);
    if ((blit.pad & 1u) != 0u) {
        let p0 = uv * size;
        return texel(base, i32(floor(p0.x)), i32(floor(p0.y)));
    }
    let out = vec2<f32>(f32(blit.out_width), f32(blit.out_height));
    let fp = size / out;
    let t = uv * size;
    // The framebuffer holds two half-line-interleaved fields, so any element the
    // game draws once per field sits twice, half a native line apart. Below the
    // native line rate that is what the TV (and PCSX2) shows; resolving it at
    // the internal scale instead makes every 1-line detail look doubled. So the
    // vertical axis is averaged over ~one native line, the horizontal axis is
    // left alone (that is where the extra sharpness lives).
    let vblur = f32((blit.pad >> 16u) & 0xFFu) / 256.0;
    let dy = max(scale, 1.0) * vblur;
    var sum = vec4<f32>(0.0);
    for (var j = 0u; j < 2u; j = j + 1u) {
        let py = t.y + select(-dy, dy, j == 1u);
        for (var i = 0u; i < 4u; i = i + 1u) {
            sum = sum + texel_linear(base, vec2<f32>(tap_pos(t.x, fp.x, i), py));
        }
    }
    return sum * 0.125;
}

fn luma3(c: vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.299, 0.587, 0.114)); }

// FXAA-style edge blend on the presented image. Bit 26 enables it; bits 27-28
// select the quality: 0 = 4 taps, 1 = 8 taps, 2 = 12 taps. The menu overlay is
// composited afterwards, so its text stays crisp.
fn fxaa(uv: vec2<f32>, c: vec4<f32>, quality: u32) -> vec4<f32> {
    let tx = 1.0 / f32(max(blit.out_width, 1u));
    let ty = 1.0 / f32(max(blit.out_height, 1u));
    let n = sample(blit.current, uv + vec2<f32>(0.0, -ty));
    let s = sample(blit.current, uv + vec2<f32>(0.0,  ty));
    let w = sample(blit.current, uv + vec2<f32>(-tx, 0.0));
    let e = sample(blit.current, uv + vec2<f32>( tx, 0.0));
    let lc = luma3(c.rgb);
    var mn = min(lc, min(min(luma3(n.rgb), luma3(s.rgb)), min(luma3(w.rgb), luma3(e.rgb))));
    var mx = max(lc, max(max(luma3(n.rgb), luma3(s.rgb)), max(luma3(w.rgb), luma3(e.rgb))));
    var sum = n + s + w + e;
    var taps = 4.0;
    if (quality >= 2u) {
        let ne = sample(blit.current, uv + vec2<f32>( tx, -ty));
        let nw = sample(blit.current, uv + vec2<f32>(-tx, -ty));
        let se = sample(blit.current, uv + vec2<f32>( tx,  ty));
        let sw = sample(blit.current, uv + vec2<f32>(-tx,  ty));
        sum += ne + nw + se + sw;
        taps += 4.0;
        mn = min(mn, min(min(luma3(ne.rgb), luma3(nw.rgb)), min(luma3(se.rgb), luma3(sw.rgb))));
        mx = max(mx, max(max(luma3(ne.rgb), luma3(nw.rgb)), max(luma3(se.rgb), luma3(sw.rgb))));
    }
    if (quality >= 3u) {
        let o = vec2<f32>(tx * 2.0, ty * 2.0);
        let n2 = sample(blit.current, uv + vec2<f32>(0.0, -o.y));
        let s2 = sample(blit.current, uv + vec2<f32>(0.0,  o.y));
        let w2 = sample(blit.current, uv + vec2<f32>(-o.x, 0.0));
        let e2 = sample(blit.current, uv + vec2<f32>( o.x, 0.0));
        sum += n2 + s2 + w2 + e2;
        taps += 4.0;
        mn = min(mn, min(min(luma3(n2.rgb), luma3(s2.rgb)), min(luma3(w2.rgb), luma3(e2.rgb))));
        mx = max(mx, max(max(luma3(n2.rgb), luma3(s2.rgb)), max(luma3(w2.rgb), luma3(e2.rgb))));
    }
    let contrast = mx - mn;
    let threshold = select(0.08, select(0.06, 0.05, quality >= 3u), quality >= 2u);
    if (contrast < threshold) { return c; }
    let avg = sum / taps;
    let k = clamp(contrast * 1.6, 0.0, select(0.45, select(0.50, 0.55, quality >= 3u), quality >= 2u));
    return vec4<f32>(mix(c.rgb, avg.rgb, k), c.a);
}

@fragment
fn fs(in: VertexOut) -> @location(0) vec4<f32> {
    var c = sample(blit.current, in.uv);
    if (blit.blend != 0u) {
        c = (c + sample(blit.previous, in.uv)) * 0.5;
    }
    // bit 26 of pad: optional FXAA on the game image; bits 27-28 = quality.
    if ((blit.pad & 0x04000000u) != 0u) {
        c = fxaa(in.uv, c, (blit.pad >> 27u) & 3u);
    }
    // bit 24 of pad: an overlay texture is active. bit 25: it is the full
    // settings menu, so dim the game behind it. The texture is a fixed
    // 1920x1080 16:9 canvas fitted into the viewport, which keeps the UI
    // undistorted at 4:3/16:9/21:9/32:9 without any CPU scaling pass.
    if ((blit.pad & 0x01000000u) != 0u) {
        if ((blit.pad & 0x02000000u) != 0u) {
            c = vec4<f32>(c.rgb * 0.2, 1.0);
        }
        let vp = vec2<f32>(f32(blit.out_width), f32(blit.out_height));
        let tex = vec2<f32>(textureDimensions(menu_texture));
        let scale = min(vp.x / tex.x, vp.y / tex.y);
        let rect = tex * scale;
        let p = in.uv * vp - (vp - rect) * 0.5;
        if (p.x >= 0.0 && p.y >= 0.0 && p.x < rect.x && p.y < rect.y) {
            let m = textureSampleLevel(menu_texture, menu_sampler, p / rect, 0.0);
            c = vec4<f32>(c.rgb * (1.0 - m.a) + m.rgb * m.a, 1.0);
        }
    }
    return vec4<f32>(c.rgb, 1.0);
}

// Second presentation pass: the heavy blit above runs at the configured
// Resolution (e.g. 1920x1080). When the window is maximized larger, this cheap
// pass scales that fixed-size image to the window instead of running FXAA and
// the multi-tap source sampling at the window resolution.
@group(0) @binding(4) var up_tex: texture_2d<f32>;
@group(0) @binding(5) var up_sampler: sampler;

@fragment
fn fs_up(in: VertexOut) -> @location(0) vec4<f32> {
    let c = textureSample(up_tex, up_sampler, in.uv);
    return vec4<f32>(c.rgb, 1.0);
}
