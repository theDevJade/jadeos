import JadePortfolioModule from './jadeportfolio.js';

const mod = await JadePortfolioModule();

let jadeDomInputBound = false;

// shader stupidity!

const SH_VP = /* wgsl */`
struct Vp { size: vec2f }
@group(0) @binding(0) var<uniform> vp: Vp;
fn ndc(px: vec2f) -> vec4f {
  return vec4f(px.x/vp.size.x*2.0-1.0, 1.0-px.y/vp.size.y*2.0, 0.0, 1.0);
}
const QUAD = array<vec2f,4>(vec2f(0,0),vec2f(1,0),vec2f(0,1),vec2f(1,1));
struct Vc { @builtin(position) pos: vec4f, @location(0) col: vec4f }
`;

// Rect: instanced quads. Instance = [x,y,w,h, r,g,b,a].
const SH_RECT = SH_VP + /* wgsl */`
struct RI { @location(0) rect: vec4f, @location(1) col: vec4f }
@vertex fn vs(@builtin(vertex_index) vi: u32, i: RI) -> Vc {
  return Vc(ndc(i.rect.xy + QUAD[vi]*i.rect.zw), i.col);
}
@fragment fn fs(v: Vc) -> @location(0) vec4f { return v.col; }
`;

// Line: instanced quads expanded from 2 endpoints. Instance = [x0,y0,x1,y1, r,g,b,a].
const SH_LINE = SH_VP + /* wgsl */`
struct LI { @location(0) p0: vec2f, @location(1) p1: vec2f, @location(2) col: vec4f }
@vertex fn vs(@builtin(vertex_index) vi: u32, i: LI) -> Vc {
  let d = i.p1 - i.p0;
  let ln = length(d);
  let dir = select(vec2f(1,0), d/ln, ln > 0.0001);
  let n = vec2f(-dir.y, dir.x) * 1.0;
  let pts = array<vec2f,4>(i.p0+n, i.p0-n, i.p1+n, i.p1-n);
  return Vc(ndc(pts[vi]), i.col);
}
@fragment fn fs(v: Vc) -> @location(0) vec4f { return v.col; }
`;

// Circle: instanced quads with SDF discard. Instance = [cx,cy,r,inner_r, r,g,b,a].
const SH_CIRC = SH_VP + /* wgsl */`
struct CI { @location(0) cp: vec2f, @location(1) rad: vec2f, @location(2) col: vec4f }
struct CVo { @builtin(position) pos: vec4f, @location(0) col: vec4f,
             @location(1) uv: vec2f, @location(2) rad: vec2f }
@vertex fn vs(@builtin(vertex_index) vi: u32, i: CI) -> CVo {
  let q = QUAD[vi]*2.0-1.0;
  return CVo(ndc(i.cp + q*i.rad.x), i.col, q, i.rad);
}
@fragment fn fs(v: CVo) -> @location(0) vec4f {
  let d = length(v.uv);
  if d > 1.0 { discard; }
  if v.rad.y > 0.0 && d < v.rad.y/v.rad.x { discard; }
  return v.col;
}
`;

// Text: instanced textured quads sampling a greyscale atlas.
// Instance = [sx0,sy0,sx1,sy1, u0,v0,u1,v1, r,g,b,a].
const SH_TEXT = SH_VP + /* wgsl */`
@group(0) @binding(1) var atlasTex: texture_2d<f32>;
@group(0) @binding(2) var atlasSmp: sampler;
struct GI { @location(0) sr: vec4f, @location(1) uv: vec4f, @location(2) col: vec4f }
struct GVo { @builtin(position) pos: vec4f,
             @location(0) uv: vec2f, @location(1) col: vec4f }
@vertex fn vs(@builtin(vertex_index) vi: u32, i: GI) -> GVo {
  let q = QUAD[vi];
  return GVo(ndc(mix(i.sr.xy,i.sr.zw,q)), mix(i.uv.xy,i.uv.zw,q), i.col);
}
@fragment fn fs(v: GVo) -> @location(0) vec4f {
  let a = textureSample(atlasTex, atlasSmp, v.uv).r;
  return vec4f(v.col.rgb, v.col.a * a);
}
`;

// Full-colour blit (Freedoom). Instance = [x,y,w,h] only (see BLIT_BUF stride 16).
const SH_BLIT = SH_VP + /* wgsl */`
@group(0) @binding(1) var blitTex: texture_2d<f32>;
@group(0) @binding(2) var blitSmp: sampler;
struct BI { @location(0) rect: vec4f }
struct BVo { @builtin(position) pos: vec4f, @location(0) uv: vec2f }
@vertex fn vs(@builtin(vertex_index) vi: u32, i: BI) -> BVo {
  let q = QUAD[vi];
  let px = i.rect.xy + q * i.rect.zw;
  return BVo(ndc(px), q);
}
@fragment fn fs(v: BVo) -> @location(0) vec4f {
  return textureSample(blitTex, blitSmp, v.uv);
}
`;

const ALPHA_BLEND = {
  color: { srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha', operation: 'add' },
  alpha: { srcFactor: 'one', dstFactor: 'one-minus-src-alpha', operation: 'add' },
};

function mkPipe(device, format, wgsl, bufLayouts, bgl, blend) {
  const mod = device.createShaderModule({ code: wgsl });
  return device.createRenderPipeline({
    layout: device.createPipelineLayout({ bindGroupLayouts: [bgl] }),
    vertex: { module: mod, entryPoint: 'vs', buffers: bufLayouts },
    fragment: {
      module: mod,
      entryPoint: 'fs',
      targets: [{ format, blend: blend || undefined }],
    },
    primitive: { topology: 'triangle-strip' },
  });
}

const RECT_BUF = [{ arrayStride: 32, stepMode: 'instance', attributes: [
  { shaderLocation: 0, offset: 0, format: 'float32x4' },
  { shaderLocation: 1, offset: 16, format: 'float32x4' },
]}];
const BLIT_BUF = [{ arrayStride: 16, stepMode: 'instance', attributes: [
  { shaderLocation: 0, offset: 0, format: 'float32x4' },
]}];
const LINE_BUF = [{ arrayStride: 32, stepMode: 'instance', attributes: [
  { shaderLocation: 0, offset: 0, format: 'float32x2' },
  { shaderLocation: 1, offset: 8, format: 'float32x2' },
  { shaderLocation: 2, offset: 16, format: 'float32x4' },
]}];
const CIRCLE_BUF = LINE_BUF; // same layout: 2+2+4 floats
const GLYPH_BUF = [{ arrayStride: 48, stepMode: 'instance', attributes: [
  { shaderLocation: 0, offset: 0, format: 'float32x4' },
  { shaderLocation: 1, offset: 16, format: 'float32x4' },
  { shaderLocation: 2, offset: 32, format: 'float32x4' },
]}];

(() => {
  const saved = localStorage.getItem('jadeos_ram_mib');
  if (saved) {
    const sel = document.getElementById('mem-select');
    if (sel) sel.value = saved;
  }
})();

document.getElementById('boot-btn').addEventListener('click', async () => {
  const memMib = parseInt(document.getElementById('mem-select').value, 10);
  // Persist RAM selection.
  localStorage.setItem('jadeos_ram_mib', String(memMib));
  const dpr = window.devicePixelRatio || 1;
  let fbW = Math.round(window.innerWidth * dpr);
  let fbH = Math.round(window.innerHeight * dpr);

  const canvas = document.getElementById('screen');
  canvas.width = fbW;
  canvas.height = fbH;
  canvas.style.width = window.innerWidth + 'px';
  canvas.style.height = window.innerHeight + 'px';

  const ui = document.getElementById('boot-ui');
  const btn = document.getElementById('boot-btn');
  btn.textContent = 'Loading';
  btn.classList.add('dots');
  btn.disabled = true;

  let gpuDevice = null;
  let gpuCtx = null;
  let gpuFmt = null;
  let ctx2d = null;
  let imgData = null;
  const useWebGPU = !!navigator.gpu;

  if (useWebGPU) {
    try {
      const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
      if (adapter) {
        gpuDevice = await adapter.requestDevice();
        gpuFmt = navigator.gpu.getPreferredCanvasFormat();
        gpuCtx = canvas.getContext('webgpu');
        gpuCtx.configure({ device: gpuDevice, format: gpuFmt, alphaMode: 'opaque' });
      }
    } catch (e) {
      console.warn('WebGPU init failed, falling back to 2D:', e);
      gpuDevice = null;
    }
  }

  if (!gpuDevice) {
    ctx2d = canvas.getContext('2d');
    imgData = ctx2d.createImageData(fbW, fbH);
  }

  // Load font into WASM memory. Fancy? No. Effective? Yes.
  try {
    const resp = await fetch('./Hack-Regular.ttf');
    if (resp.ok) {
      const buf = await resp.arrayBuffer();
      const bytes = new Uint8Array(buf);
      const ptr = mod.allocFontBuffer(bytes.length);
      mod.HEAPU8.set(bytes, ptr);
      mod.commitFontBuffer(dpr);
    } else {
      console.warn('Hack-Regular.ttf fetch failed:', resp.status);
    }
  } catch (e) {
    console.warn('Font load error:', e);
  }

  try {
    if (mod.FS) {
      const iw = await fetch('./freedoom1.wad');
      if (iw.ok) {
        mod.FS.writeFile('/freedoom1.wad', new Uint8Array(await iw.arrayBuffer()));
        mod.setFreedoomIwadReady(true);
      }
    }
  } catch (e) {
    console.warn('Freedoom IWAD load:', e);
  }

  try {
    if (mod.FS && typeof mod.setMediaAssetsReady === 'function') {
      try {
        mod.FS.mkdir('/media');
      } catch (_) {
        /* exists */
      }
      const png = await fetch('./media/amazingimage.png');
      if (png.ok) mod.FS.writeFile('/media/amazingimage.png', new Uint8Array(await png.arrayBuffer()));
      if (png.ok) mod.setMediaAssetsReady(true);
    }
  } catch (e) {
    console.warn('Media assets load:', e);
  }

  if (gpuDevice) mod.setWebGpuMode(true);
  mod.boot(memMib, fbW, fbH, dpr);

  let audioCtx = null;
  let audioNextT = 0;
  try {
    const sr = typeof mod.audioSampleRate === 'function' ? mod.audioSampleRate() : 44100;
    audioCtx = new AudioContext({ sampleRate: sr });
  } catch (_) {
    audioCtx = new AudioContext();
  }
  await audioCtx.resume().catch(() => {});

  let pipes = null;
  let bglBase = null;
  let bglText = null;
  let bgBase = null;
  const atlasTex = [];
  const atlasInfo = [];
  let vpBuf = null;
  // Persistent instance staging buffer (4 MiB, grown on demand).
  let instBuf = null;
  let instBufSize = 0;

  if (gpuDevice) {
    bglBase = gpuDevice.createBindGroupLayout({ entries: [
      { binding: 0, visibility: GPUShaderStage.VERTEX, buffer: { type: 'uniform' } },
    ]});
    bglText = gpuDevice.createBindGroupLayout({ entries: [
      { binding: 0, visibility: GPUShaderStage.VERTEX, buffer: { type: 'uniform' } },
      { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
      { binding: 2, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
    ]});
    const bglBlit = gpuDevice.createBindGroupLayout({ entries: [
      { binding: 0, visibility: GPUShaderStage.VERTEX, buffer: { type: 'uniform' } },
      { binding: 1, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType: 'float' } },
      { binding: 2, visibility: GPUShaderStage.FRAGMENT, sampler: { type: 'filtering' } },
    ]});

    pipes = {
      rect: mkPipe(gpuDevice, gpuFmt, SH_RECT, RECT_BUF, bglBase, null),
      recta: mkPipe(gpuDevice, gpuFmt, SH_RECT, RECT_BUF, bglBase, ALPHA_BLEND),
      line: mkPipe(gpuDevice, gpuFmt, SH_LINE, LINE_BUF, bglBase, ALPHA_BLEND),
      circ: mkPipe(gpuDevice, gpuFmt, SH_CIRC, CIRCLE_BUF, bglBase, ALPHA_BLEND),
      text: mkPipe(gpuDevice, gpuFmt, SH_TEXT, GLYPH_BUF, bglText, ALPHA_BLEND),
      blit: mkPipe(gpuDevice, gpuFmt, SH_BLIT, BLIT_BUF, bglBlit, null),
    };

    vpBuf = gpuDevice.createBuffer({ size: 8, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    gpuDevice.queue.writeBuffer(vpBuf, 0, new Float32Array([fbW, fbH]));

    bgBase = gpuDevice.createBindGroup({
      layout: bglBase,
      entries: [{ binding: 0, resource: { buffer: vpBuf } }],
    });

    const atlSampler = gpuDevice.createSampler({ minFilter: 'linear', magFilter: 'linear' });
    const blitSampler = gpuDevice.createSampler({ minFilter: 'linear', magFilter: 'linear' });
    let doomBlitTex = null;
    let doomBlitTw = 0;
    let doomBlitTh = 0;
    const blitVBuf = gpuDevice.createBuffer({
      size: 64,
      usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
    });
    let blitBind = null;

    function ensureDoomBlitTexture(tw, th) {
      if (doomBlitTex && doomBlitTw >= tw && doomBlitTh >= th) return;
      doomBlitTex?.destroy();
      doomBlitTw = Math.max(tw, 512);
      doomBlitTh = Math.max(th, 256);
      doomBlitTex = gpuDevice.createTexture({
        size: [doomBlitTw, doomBlitTh, 1],
        format: 'rgba8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
      });
      blitBind = gpuDevice.createBindGroup({
        layout: bglBlit,
        entries: [
          { binding: 0, resource: { buffer: vpBuf } },
          { binding: 1, resource: doomBlitTex.createView() },
          { binding: 2, resource: blitSampler },
        ],
      });
    }

    function queueWriteRgbaBlit(queue, tex, bw, bh, rgba) {
      const tight = bw * 4;
      const bpr = Math.ceil(tight / 256) * 256;
      if (bpr === tight) {
        queue.writeTexture(
          { texture: tex, origin: { x: 0, y: 0, z: 0 } },
          rgba,
          { bytesPerRow: bpr, rowsPerImage: bh },
          { width: bw, height: bh, depthOrArrayLayers: 1 },
        );
      } else {
        const padded = new Uint8Array(bpr * bh);
        for (let y = 0; y < bh; y++) {
          padded.set(rgba.subarray(y * tight, y * tight + tight), y * bpr);
        }
        queue.writeTexture(
          { texture: tex, origin: { x: 0, y: 0, z: 0 } },
          padded,
          { bytesPerRow: bpr, rowsPerImage: bh },
          { width: bw, height: bh, depthOrArrayLayers: 1 },
        );
      }
    }

    // Upload all 3 font atlas sizes as R8Unorm textures.
    for (let sid = 0; sid < 3; sid++) {
      const info = mod.getAtlasInfo(sid);
      atlasInfo[sid] = info;
      if (!info.loaded || !info.width) continue;
      const bmp = mod.getAtlasBitmap(sid);
      if (!bmp) continue;

      const tex = gpuDevice.createTexture({
        size: [info.width, info.height],
        format: 'r8unorm',
        usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST,
      });
      gpuDevice.queue.writeTexture(
        { texture: tex },
        bmp,
        { bytesPerRow: info.width, rowsPerImage: info.height },
        [info.width, info.height],
      );
      atlasTex[sid] = tex;
      atlasTex[sid].bg = gpuDevice.createBindGroup({
        layout: bglText,
        entries: [
          { binding: 0, resource: { buffer: vpBuf } },
          { binding: 1, resource: tex.createView() },
          { binding: 2, resource: atlSampler },
        ],
      });
    }

    const _ripBuf = new ArrayBuffer(4);
    const _ripF32 = new Float32Array(_ripBuf);
    const _ripU32 = new Uint32Array(_ripBuf);
    // JavaScript has no clean float<->uint bitcast, so this tiny hack exists.
    function f2f(bits) {
      _ripU32[0] = bits;
      return _ripF32[0];
    }

    function ensureInstBuf(needBytes) {
      const maxBuf = gpuDevice.limits.maxBufferSize ?? 268435456;
      const aligned = Math.min(
        Math.ceil(Math.max(needBytes, 1) / 256) * 256,
        maxBuf,
      );
      if (aligned > instBufSize) {
        instBuf?.destroy();
        instBufSize = Math.min(
          Math.max(aligned * 2, 4 * 1024 * 1024),
          maxBuf,
        );
        instBuf = gpuDevice.createBuffer({
          size: instBufSize,
          usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST,
        });
      }
    }

    // Convert flat glyph metric arrays into something less painful to index.
    const glyphM = atlasInfo.map((info) => {
      if (!info?.glyphMetrics) return null;
      const m = new Float32Array(info.glyphMetrics.length);
      m.set(info.glyphMetrics);
      return { m, w: info.width, h: info.height };
    });

    // Indexed by type: 0=rect 1=recta 2=line 3=circ 4=text
    const FP_ARR = [8, 8, 8, 8, 12];
    const TYPE_IDS = ['rect', 'recta', 'line', 'circ', 'text'];
    const MAX_BATCHES = 512;
    // Each batch: [ti, sid, sx, sy, sw, sh, f32Start, f32Count]
    const batchBuf = new Int32Array(MAX_BATCHES * 8);
    let stagingF32 = new Float32Array(256 * 1024); // 1 MiB initial
    const dec = new TextDecoder();

    // Yes this naming is ugly. No, we're not pretending it's elegant.
    let _nB = 0;
    let _nF = 0;
    let _cT = -1;
    let _cS = 0;
    let _cSx = 0;
    let _cSy = 0;
    let _cSw = 0;
    let _cSh = 0;
    let _sx = 0;
    let _sy = 0;
    let _sw = 0;
    let _sh = 0;

    function _closeBatch() {
      if (_nB > 0) batchBuf[(_nB - 1) * 8 + 7] = _nF - batchBuf[(_nB - 1) * 8 + 6];
    }

    // Batch state machine. I still have little to no idea how this works.
    function _openBatch(ti, sid) {
      if (_nB >= MAX_BATCHES) return;
      const bb = _nB++ * 8;
      batchBuf[bb] = ti;
      batchBuf[bb + 1] = sid;
      batchBuf[bb + 2] = _sx;
      batchBuf[bb + 3] = _sy;
      batchBuf[bb + 4] = _sw;
      batchBuf[bb + 5] = _sh;
      batchBuf[bb + 6] = _nF;
      batchBuf[bb + 7] = 0;
      _cT = ti;
      _cS = sid;
      _cSx = _sx;
      _cSy = _sy;
      _cSw = _sw;
      _cSh = _sh;
    }

    function _eb(ti, sid) {
      if (_cT !== ti || _cS !== sid || _sx !== _cSx || _sy !== _cSy || _sw !== _cSw || _sh !== _cSh) {
        _closeBatch();
        _openBatch(ti, sid);
      }
    }

    window._webgpuRender = function() {
      function drawBatchSlice(pass, bi0, bi1) {
        let lsx = -1;
        let lsy = -1;
        let lsw = -1;
        let lsh = -1;
        for (let bi = bi0; bi < bi1; bi++) {
          const bb = bi * 8;
          const ti = batchBuf[bb];
          const sid = batchBuf[bb + 1];
          const sx = batchBuf[bb + 2];
          const sy = batchBuf[bb + 3];
          const sw = batchBuf[bb + 4];
          const sh = batchBuf[bb + 5];
          const f0 = batchBuf[bb + 6];
          const fc = batchBuf[bb + 7];
          if (sw <= 0 || sh <= 0 || fc <= 0) continue;
          const cnt = (fc / FP_ARR[ti]) | 0;
          if (cnt < 1) continue;
          if (sx !== lsx || sy !== lsy || sw !== lsw || sh !== lsh) {
            pass.setScissorRect(sx, sy, sw, sh);
            lsx = sx;
            lsy = sy;
            lsw = sw;
            lsh = sh;
          }
          pass.setPipeline(pipes[TYPE_IDS[ti]]);
          pass.setBindGroup(0, ti === 4 ? (atlasTex[sid]?.bg || bgBase) : bgBase);
          pass.setVertexBuffer(0, instBuf, f0 * 4, fc * 4);
          pass.draw(4, cnt);
        }
      }

      const cmds = mod.getCmdBuf();
      const pool = mod.getStrPool();
      const blitPool = mod.getBlitPool();
      if (!cmds) return;
      const n = cmds.length >>> 3;

      // Pull CLEAR color for render-pass clearValue.
      let cr = 0.02;
      let cg = 0.035;
      let cb = 0.063;
      let ca = 1;
      for (let i = 0; i < n; i++) {
        if ((cmds[i << 3] & 0xFF) === 0x01) {
          const c = cmds[(i << 3) + 5];
          cr = ((c >> 16) & 255) / 255;
          cg = ((c >> 8) & 255) / 255;
          cb = (c & 255) / 255;
          ca = ((c >> 24) & 255) / 255;
          break;
        }
      }

      // Reset batch state.
      _nB = 0;
      _nF = 0;
      _cT = -1;
      _cS = 0;
      _cSx = 0;
      _cSy = 0;
      _cSw = fbW;
      _cSh = fbH;
      _sx = 0;
      _sy = 0;
      _sw = fbW;
      _sh = fbH;

      let textFloatBudget = 0;
      for (let pi = 0; pi < n; pi++) {
        const t = cmds[pi << 3] & 0xff;
        if (t === 0x80) textFloatBudget += ((cmds[pi << 3] >> 16) & 0xffff) * 12;
      }

      const needF = n * 12 + textFloatBudget + 4096;
      if (needF > stagingF32.length) {
        stagingF32 = new Float32Array(
          Math.max(needF + 65536, (stagingF32.length || 1) * 2),
        );
      }
      let sf = stagingF32;

      const segments = [];
      let segB0 = 0;

      // Giant command decoder. Pretty? No. Good enough i guess? Yes.
      for (let i = 0; i < n; i++) {
        const b = i << 3;
        const tf = cmds[b];
        const type = tf & 0xFF;
        const sid = (tf >> 8) & 0xFF;
        const slen = (tf >> 16) & 0xFFFF;
        const x0 = f2f(cmds[b + 1]);
        const y0 = f2f(cmds[b + 2]);
        const x1 = f2f(cmds[b + 3]);
        const y1 = f2f(cmds[b + 4]);
        const col = cmds[b + 5];
        const r = ((col >> 16) & 255) / 255;
        const g = ((col >> 8) & 255) / 255;
        const bv = (col & 255) / 255;
        const a = ((col >> 24) & 255) / 255;

        switch (type) {
          case 0x01:
            break;
          case 0x04:
            _eb(0, 0);
            sf[_nF++] = x0;
            sf[_nF++] = y0;
            sf[_nF++] = x1;
            sf[_nF++] = y1;
            sf[_nF++] = r;
            sf[_nF++] = g;
            sf[_nF++] = bv;
            sf[_nF++] = a;
            break;
          case 0x07:
            _eb(1, 0);
            sf[_nF++] = x0;
            sf[_nF++] = y0;
            sf[_nF++] = x1;
            sf[_nF++] = y1;
            sf[_nF++] = r;
            sf[_nF++] = g;
            sf[_nF++] = bv;
            sf[_nF++] = a;
            break;
          case 0x03:
          case 0x09:
            _eb(2, 0);
            sf[_nF++] = x0;
            sf[_nF++] = y0;
            sf[_nF++] = x1;
            sf[_nF++] = y1;
            sf[_nF++] = r;
            sf[_nF++] = g;
            sf[_nF++] = bv;
            sf[_nF++] = a;
            break;
          case 0x08:
            _eb(3, 0);
            sf[_nF++] = x0;
            sf[_nF++] = y0;
            sf[_nF++] = x1;
            sf[_nF++] = y1;
            sf[_nF++] = r;
            sf[_nF++] = g;
            sf[_nF++] = bv;
            sf[_nF++] = a;
            break;
          case 0x06: {
            _closeBatch();
            const dx = Math.round(f2f(cmds[b + 1]));
            const dy = Math.round(f2f(cmds[b + 2]));
            const bw = Math.max(1, Math.round(f2f(cmds[b + 3])) | 0);
            const bh = Math.max(1, Math.round(f2f(cmds[b + 4])) | 0);
            const pOff = cmds[b + 5] >>> 0;
            const pBytes = cmds[b + 6] >>> 0;
            segments.push({
              b0: segB0,
              b1: _nB,
              blit: {
                dx,
                dy,
                bw,
                bh,
                pOff,
                pBytes,
                sx: _sx,
                sy: _sy,
                sw: _sw,
                sh: _sh,
              },
            });
            segB0 = _nB;
            break;
          }
          case 0x80: {
            // TEXT
            const soff = cmds[b + 6];
            if (!pool || !glyphM[sid]) break;
            _eb(4, sid);
            const text = dec.decode(pool.subarray(soff, soff + slen));
            if (_nF + text.length * 14 > sf.length) {
              const nl = Math.max(sf.length * 2, _nF + text.length * 14 + 65536);
              const ns = new Float32Array(nl);
              ns.set(sf);
              stagingF32 = ns;
              sf = stagingF32;
            }
            const gm = glyphM[sid];
            const m = gm.m;
            let px = x0;
            for (let ci = 0; ci < text.length; ci++) {
              const idx = text.charCodeAt(ci) - 0x20;
              if (idx < 0 || idx >= 96) {
                px += 8;
                continue;
              }
              const j = idx * 9;
              const gx0 = px + m[j];
              const gy0 = y0 + m[j + 1];
              const gx1 = px + m[j + 2];
              const gy1 = y0 + m[j + 3];
              if (gx1 > gx0 && gy1 > gy0) {
                sf[_nF++] = gx0;
                sf[_nF++] = gy0;
                sf[_nF++] = gx1;
                sf[_nF++] = gy1;
                sf[_nF++] = m[j + 5] / gm.w;
                sf[_nF++] = m[j + 6] / gm.h;
                sf[_nF++] = m[j + 7] / gm.w;
                sf[_nF++] = m[j + 8] / gm.h;
                sf[_nF++] = r;
                sf[_nF++] = g;
                sf[_nF++] = bv;
                sf[_nF++] = a;
              }
              px += m[j + 4];
            }
            break;
          }
          case 0x0A:
            _closeBatch();
            _cT = -1;
            _sx = Math.max(0, Math.round(x0));
            _sy = Math.max(0, Math.round(y0));
            _sw = Math.max(1, Math.min(Math.round(x1) - _sx, fbW - _sx));
            _sh = Math.max(1, Math.min(Math.round(y1) - _sy, fbH - _sy));
            break;
          case 0x0B:
            _closeBatch();
            _cT = -1;
            _sx = 0;
            _sy = 0;
            _sw = fbW;
            _sh = fbH;
            break;
          case 0xFF:
            break;
        }
      }

      _closeBatch();
      segments.push({ b0: segB0, b1: _nB, blit: null });

      const instBytes = _nF * 4;
      ensureInstBuf(Math.max(instBytes, 256));
      if (_nF > 0 && instBuf) {
        const cap = Number(instBuf.size);
        let wb = Math.min(instBytes, sf.byteLength, cap);
        wb = wb & ~3;
        if (wb > 0) {
          const CHUNK = 256 * 1024;
          for (let off = 0; off < wb;) {
            let slice = Math.min(CHUNK, wb - off);
            slice -= slice % 4;
            if (slice <= 0) break;
            gpuDevice.queue.writeBuffer(instBuf, off, sf, off, slice);
            off += slice;
          }
        }
      }

      const enc = gpuDevice.createCommandEncoder();
      const pass = enc.beginRenderPass({
        colorAttachments: [{
          view: gpuCtx.getCurrentTexture().createView(),
          loadOp: 'clear',
          clearValue: { r: cr, g: cg, b: cb, a: ca },
          storeOp: 'store',
        }],
      });
      pass.setViewport(0, 0, fbW, fbH, 0, 1);

      for (const seg of segments) {
        drawBatchSlice(pass, seg.b0, seg.b1);
        if (!seg.blit) continue;
        const B = seg.blit;
        if (!blitPool || B.pBytes < B.bw * B.bh * 4) continue;
        ensureDoomBlitTexture(B.bw, B.bh);
        queueWriteRgbaBlit(
          gpuDevice.queue,
          doomBlitTex,
          B.bw,
          B.bh,
          blitPool.subarray(B.pOff, B.pOff + B.pBytes),
        );
        gpuDevice.queue.writeBuffer(
          blitVBuf,
          0,
          new Float32Array([B.dx, B.dy, B.bw, B.bh]),
        );
        pass.setScissorRect(B.sx, B.sy, B.sw, B.sh);
        pass.setPipeline(pipes.blit);
        pass.setBindGroup(0, blitBind);
        pass.setVertexBuffer(0, blitVBuf, 0, 16);
        pass.draw(4, 1);
      }

      pass.end();
      gpuDevice.queue.submit([enc.finish()]);
    };
  } // end WebGPU setup

  ui.style.display = 'none';
  canvas.style.display = 'block';
  canvas.focus();

  const clipPreloadBox = document.getElementById('clip-preload');
  const clipPreloadText = document.getElementById('clip-preload-text');
  const clipPreloadBar = document.getElementById('clip-preload-bar');

  void (async () => {
    const clipReady = () => {
      try {
        if (typeof mod.setMediaClipReady === 'function') mod.setMediaClipReady(true);
      } catch (e) {
        console.warn('setMediaClipReady:', e);
      }
      if (clipPreloadBox) clipPreloadBox.style.display = 'none';
    };

    const clipProgress = (label, cur, max) => {
      if (clipPreloadText) clipPreloadText.textContent = label;
      if (clipPreloadBar) {
        clipPreloadBar.max = Math.max(1, max | 0);
        clipPreloadBar.value = Math.min(clipPreloadBar.max, Math.max(0, cur | 0));
      }
      if (clipPreloadBox) clipPreloadBox.style.display = 'flex';
    };

    function formatClipFrameFile(pattern, index) {
      const m = pattern.match(/%0*(\d+)d/);
      if (m) {
        const w = parseInt(m[1], 10);
        return pattern.replace(/%0*\d+d/, String(index).padStart(w, '0'));
      }
      if (pattern.includes('%d')) return pattern.replace(/%d/, String(index));
      return pattern;
    }

    if (!mod.FS || typeof mod.setMediaClipReady !== 'function') {
      clipReady();
      return;
    }

    const BATCH = 14;
    try {
      try {
        mod.FS.mkdir('/media');
      } catch (_) {
        /* exists */
      }
      try {
        mod.FS.mkdir('/media/badapple');
      } catch (_) {
        /* exists */
      }

      let seqResp = await fetch('./media/badapple/sequence.txt');
      if (!seqResp.ok) {
        seqResp = await fetch('./media/badapple/sequence.example.txt');
      }
      if (!seqResp.ok) {
        return;
      }

      const seqText = await seqResp.text();
      const lines = seqText
        .split(/\r?\n/)
        .map((s) => s.trim())
        .filter((s) => s && s[0] !== '#');
      if (lines.length < 3) {
        console.warn('Bad Apple clip: sequence.txt too short');
        return;
      }

      const pattern = lines[1];
      const frameCount = parseInt(lines[2], 10);
      if (!Number.isFinite(frameCount) || frameCount < 1 || frameCount > 500000) {
        console.warn('Bad Apple clip: bad frame count');
        return;
      }

      const audioLine = lines.length >= 4 ? lines[3] : '';
      const totalSteps = 1 + (audioLine ? 1 : 0) + frameCount;
      const baseFrameStep = 1 + (audioLine ? 1 : 0);

      clipProgress('Clip: reading manifest', 0, totalSteps);
      mod.FS.writeFile('/media/badapple/sequence.txt', seqText);
      clipProgress('Clip: manifest ready', 1, totalSteps);

      if (audioLine) {
        clipProgress('Clip: loading ' + audioLine, 1, totalSteps);
        const ar = await fetch('./media/badapple/' + audioLine);
        if (!ar.ok) throw new Error('audio fetch ' + ar.status);
        mod.FS.writeFile('/media/badapple/' + audioLine, new Uint8Array(await ar.arrayBuffer()));
        clipProgress('Clip: audio ready', 2, totalSteps);
      }

      for (let start = 1; start <= frameCount; start += BATCH) {
        const end = Math.min(frameCount, start + BATCH - 1);
        const tasks = [];
        for (let i = start; i <= end; i++) {
          const name = formatClipFrameFile(pattern, i);
          tasks.push((async () => {
            const r = await fetch('./media/badapple/' + name);
            if (!r.ok) throw new Error(name + ' HTTP ' + r.status);
            mod.FS.writeFile('/media/badapple/' + name, new Uint8Array(await r.arrayBuffer()));
          })());
        }
        await Promise.all(tasks);
        clipProgress('Clip: frames ' + end + '/' + frameCount, baseFrameStep + end, totalSteps);
      }
    } catch (e) {
      console.warn('Bad Apple clip preload:', e);
    } finally {
      clipReady();
    }
  })();

  function toFb(e) {
    const rect = canvas.getBoundingClientRect();
    return {
      x: Math.max(0, Math.floor((e.clientX - rect.left) * fbW / rect.width)),
      y: Math.max(0, Math.floor((e.clientY - rect.top) * fbH / rect.height)),
    };
  }

  let doomMouseLast = null;
  function doomButtonMask(e) {
    const b = e.buttons | 0;
    let m = 0;
    if (b & 1) m |= 1;
    if (b & 2) m |= 2;
    if (b & 4) m |= 4;
    return m >>> 0;
  }

  function feedDoomMouse(e) {
    if (!mod.isRunning()) return;
    const dd = window.devicePixelRatio || 1;
    let gdx = Math.round((e.movementX || 0) * dd);
    let gdy = Math.round((e.movementY || 0) * dd);
    if (gdx === 0 && gdy === 0 && doomMouseLast !== null) {
      const p = toFb(e);
      gdx = p.x - doomMouseLast.x;
      gdy = p.y - doomMouseLast.y;
    }
    doomMouseLast = toFb(e);
    mod.sendMouseGame(gdx | 0, gdy | 0, doomButtonMask(e));
  }

  if (!jadeDomInputBound) {
    jadeDomInputBound = true;
    canvas.addEventListener('mousedown', (e) => {
      if (!mod.isRunning()) return;
      e.preventDefault();
      const { x, y } = toFb(e);
      mod.sendMouseDown(x >>> 0, y >>> 0);
      canvas.focus();
      doomMouseLast = toFb(e);
      mod.sendMouseGame(0, 0, doomButtonMask(e));
      try {
        if (
          typeof mod.doomPointerLockDesired === 'function'
          && mod.doomPointerLockDesired()
          && document.pointerLockElement !== canvas
        ) {
          requestAnimationFrame(() => {
            canvas.requestPointerLock().catch(() => {});
          });
        }
      } catch (_) {}
    });
    document.addEventListener('mousemove', (e) => {
      if (!mod.isRunning()) return;
      const locked = document.pointerLockElement === canvas;
      if (!locked) {
        const { x, y } = toFb(e);
        mod.sendMouseMove(x >>> 0, y >>> 0);
      }
      feedDoomMouse(e);
    });
    document.addEventListener('mouseup', (e) => {
      if (!mod.isRunning()) return;
      mod.sendMouseUp();
      doomMouseLast = null;
      mod.sendMouseGame(0, 0, doomButtonMask(e));
    });
    canvas.addEventListener('mouseleave', () => {
      doomMouseLast = null;
    });
    canvas.addEventListener('wheel', (e) => {
      if (!mod.isRunning()) return;
      e.preventDefault();
      mod.sendScroll(e.deltaY > 0 ? 1 : -1);
    }, { passive: false });

    // Bit 20: keydown (1) vs keyup (0). Bit 21: DOM repeat (autorepeat);
    // Freedoom ignores repeat anyway because it has trust issues.
    document.addEventListener('keyup', (e) => {
      if (!mod.isRunning()) return;
      const ch = e.key.length === 1 ? e.key.charCodeAt(0) : 0;
      const mods = ((e.shiftKey ? 1 : 0))
        | ((e.ctrlKey ? 1 : 0) << 1)
        | ((e.altKey ? 1 : 0) << 2)
        | ((e.metaKey ? 1 : 0) << 3);
      const k = (((e.keyCode & 0xFFFF) | (mods << 16)) >>> 0);
      mod.sendKey(k, ch >>> 0);
      if ([8, 9, 13, 32, 37, 38, 39, 40, 46].includes(e.keyCode)) e.preventDefault();
      if (e.altKey && ['q', 'Q', ' ', 'Enter', 'Tab'].includes(e.key)) e.preventDefault();
    });
    document.addEventListener('keydown', (e) => {
      if (!mod.isRunning()) return;
      const ch = e.key.length === 1 ? e.key.charCodeAt(0) : 0;
      const mods = ((e.shiftKey ? 1 : 0))
        | ((e.ctrlKey ? 1 : 0) << 1)
        | ((e.altKey ? 1 : 0) << 2)
        | ((e.metaKey ? 1 : 0) << 3);
      const KEY_DOWN = 1 << 20;
      const KEY_REPEAT = 1 << 21;
      let k = (((e.keyCode & 0xFFFF) | (mods << 16)) | KEY_DOWN) >>> 0;
      if (e.repeat) k |= KEY_REPEAT;
      mod.sendKey(k, ch >>> 0);
      // Prevent default for navigation + our WM hotkeys.
      if ([8, 9, 13, 32, 37, 38, 39, 40, 46].includes(e.keyCode)) e.preventDefault();
      if (e.altKey && ['q', 'Q', ' ', 'Enter', 'Tab'].includes(e.key)) e.preventDefault();
    });

    // Resize handling. If we don't things get realllly wierd.
    window.addEventListener('resize', () => {
      if (!mod.isRunning()) return;
      const nd = window.devicePixelRatio || 1;
      const nw = Math.round(window.innerWidth * nd);
      const nh = Math.round(window.innerHeight * nd);
      if (nw === fbW && nh === fbH) return;
      fbW = nw;
      fbH = nh;
      canvas.width = fbW;
      canvas.height = fbH;
      canvas.style.width = window.innerWidth + 'px';
      canvas.style.height = window.innerHeight + 'px';
      mod.resize(fbW, fbH);
      if (gpuDevice) {
        gpuCtx.configure({ device: gpuDevice, format: gpuFmt, alphaMode: 'opaque' });
        gpuDevice.queue.writeBuffer(vpBuf, 0, new Float32Array([fbW, fbH]));
      } else {
        imgData = ctx2d.createImageData(fbW, fbH);
      }
    });
  }

  const CPU_CYCLES = 5000;
  const SIM_DT_MS = 1000 / 120; // C++ spring physics expects 120 Hz fixed step
  const MAX_SIM_DT = SIM_DT_MS * 2;
  let accumulator = SIM_DT_MS;
  let lastSimTime = performance.now() - SIM_DT_MS;
  let lastUnix = 0;

  const fpsEl = document.getElementById('fps');
  fpsEl.style.display = 'block';
  let fpsFrames = 0;
  let fpsLastTime = performance.now();

  function frame(now) {
    // FPS
    fpsFrames++;
    if (now - fpsLastTime >= 1000) {
      fpsEl.textContent = `${fpsFrames} FPS`;
      fpsFrames = 0;
      fpsLastTime = now;
    }

    const unix = (Date.now() / 1000) >>> 0;
    if (unix !== lastUnix) {
      mod.setWallClock(unix);
      lastUnix = unix;
    }

    // Fixed-step sim: clamp delta so stalls don't explode into backlog.
    accumulator += Math.min(now - lastSimTime, MAX_SIM_DT);
    lastSimTime = now;
    while (accumulator >= SIM_DT_MS) {
      if (mod.isRunning()) mod.tick(CPU_CYCLES);
      accumulator -= SIM_DT_MS;
    }

    try {
      if (
        mod.isRunning()
        && typeof mod.doomPointerLockDesired === 'function'
        && !mod.doomPointerLockDesired()
        && document.pointerLockElement === canvas
      ) {
        document.exitPointerLock();
      }
    } catch (_) {}

    if (audioCtx && mod.isRunning() && typeof mod.pullAudioPcmS16 === 'function') {
      const sr = typeof mod.audioSampleRate === 'function' ? mod.audioSampleRate() : 44100;
      let totalFrames = 0;
      while (totalFrames < 16384) {
        const pcm = mod.pullAudioPcmS16(4096);
        if (!pcm || pcm.length < 4) break;
        const fLen = pcm.length >> 1;
        const ab = audioCtx.createBuffer(2, fLen, sr);
        const L = ab.getChannelData(0);
        const R = ab.getChannelData(1);
        for (let i = 0; i < fLen; i++) {
          L[i] = pcm[i * 2] * (1 / 32768);
          R[i] = pcm[i * 2 + 1] * (1 / 32768);
        }
        const src = audioCtx.createBufferSource();
        src.buffer = ab;
        src.connect(audioCtx.destination);
        const t0 = Math.max(audioNextT, audioCtx.currentTime + 0.1);
        src.start(t0);
        audioNextT = t0 + fLen / sr;
        totalFrames += fLen;
      }
    }

    if (gpuDevice) {
      window._webgpuRender();
    } else {
      const fb = mod.getFramebuffer();
      imgData.data.set(new Uint8ClampedArray(fb.buffer, fb.byteOffset, fb.byteLength));
      ctx2d.putImageData(imgData, 0, 0);
    }
    requestAnimationFrame(frame);
  }

  requestAnimationFrame(frame);
});
