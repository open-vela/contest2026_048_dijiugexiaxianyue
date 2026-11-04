/*
 * vmap 地图样式表 + 绘制器（浏览器侧）
 *
 * 这里的每个 token 都要能落到设备端：RGB565、240×400、无 GPU。
 * 设备对应文件：vmap_style.c（调色板）、vmap_render.c（线宽/圆角）、
 * vmap_label.c（标注）。改这里的数值时同步看 ../STYLE.md 的移植表。
 *
 * 设计原则（对着"好看"的三条）：
 *   1. 底图安静：用地色块只做 3~6% 的色差，颜色预算全给水系和道路。
 *   2. 道路靠"壳"分层：每条路 = 深色壳 + 亮色芯，等级用宽度阶梯表达，
 *      而不是用七种颜色。放大只加宽，不换色。
 *   3. 文字压在路上：路名沿路方向旋转、白描边、按等级排优先级，
 *      地名/水名/公园名各自一套颜色与字号。
 */
"use strict";

/* 道路等级（与 build_vmap.py classify_road / vmap_format.h 一致） */
const VMAP_ROAD_NAMES = [
  null, "motorway", "primary", "secondary", "tertiary",
  "residential", "service", "path",
];

/*
 * 每份样式：
 *   bg            画布底色
 *   land[1..5]    住/商/工/校/园 的填色（透明=不画）
 *   forest        林地
 *   water         水面 {fill, edge}
 *   waterway      水系线色
 *   roads[c]      {fill, case, w}  w 是设备 1× 下的芯宽（壳 = w + caseExtra）
 *   pathDash      path/track 虚线 [实, 虚]（设备像素，null=实线）
 *   label         {place, water, poi, road, halo, park}
 *   ui            {accent} 面板强调色，仅预览页用
 */
const VMAP_STYLE_PRESETS = {
  /* 默认：暖白纸面 + 琥珀主干道 + 白支路。最接近"纸质骑行地图"的观感，
   * 也是设备 classic 的候选。 */
  paper: {
    id: "paper",
    title: "纸面",
    bg: "#F4F1E8",
    land: {
      1: "#EFEADE",
      2: "#F5ECE4",
      3: "#EFECE4",
      4: "#F9F2DA",
      5: "#D8E8CA",
    },
    forest: "#CBE0BC",
    water: { fill: "#A9D0E8", edge: "#7FB6D4" },
    waterway: "#93C2DA",
    roads: [
      null,
      { fill: "#F7C873", case: "#CE8F33", w: 4.2 },  /* motorway  */
      { fill: "#FBDC9C", case: "#C6A054", w: 3.6 },  /* primary   */
      { fill: "#FEFDF9", case: "#B7B0A1", w: 3.0 },  /* secondary */
      { fill: "#FEFDF9", case: "#C1BAAA", w: 2.6 },  /* tertiary  */
      { fill: "#FEFDF9", case: "#CAC4B6", w: 2.2 },  /* resid.    */
      { fill: "#FEFDF9", case: "#D5CFC2", w: 1.7 },  /* service   */
      { fill: "#A97C4A", case: null, w: 1.4 },       /* path      */
    ],
    pathDash: [5, 4],
    label: {
      place: "#2E2C29",
      water: "#3D7EA6",
      poi: "#3F7048",
      road: "#514C45",
      halo: "rgba(255,255,255,0.92)",
    },
    ui: { accent: "#D69F45" },
  },

  /* 冷白 + 蓝水：屏幕观感最干净的一份，对应设备 nav。 */
  bright: {
    id: "bright",
    title: "冷白",
    bg: "#F6F7F9",
    land: {
      1: "#EEF1F4",
      2: "#F6EFF0",
      3: "#EEF1F3",
      4: "#FBF3DE",
      5: "#DFEDD6",
    },
    forest: "#D3E7C4",
    water: { fill: "#A9D4F0", edge: "#7BB8DD" },
    waterway: "#8FC4E4",
    roads: [
      null,
      { fill: "#F8C556", case: "#D09425", w: 4.4 },
      { fill: "#FBD98A", case: "#C4A050", w: 3.8 },
      { fill: "#FFFFFF", case: "#BCC4CF", w: 3.1 },
      { fill: "#FFFFFF", case: "#C6CDD7", w: 2.7 },
      { fill: "#FFFFFF", case: "#CFD5DE", w: 2.2 },
      { fill: "#FFFFFF", case: "#DBE0E6", w: 1.7 },
      { fill: "#8FA0AE", case: null, w: 1.4 },
    ],
    pathDash: [5, 4],
    label: {
      place: "#1F2933",
      water: "#2F7BA8",
      poi: "#3C7048",
      road: "#4A5560",
      halo: "rgba(255,255,255,0.94)",
    },
    ui: { accent: "#D9A02F" },
  },

  /* 户外：绿更重、路面偏米白、步道棕。适合骑行/徒步场景（设备 outdoor）。 */
  terrain: {
    id: "terrain",
    title: "户外",
    bg: "#F3F0E2",
    land: {
      1: "#EDE8D6",
      2: "#F2E7DA",
      3: "#E9E7DA",
      4: "#F7EFD0",
      5: "#D2E5BE",
    },
    forest: "#C2DCAE",
    water: { fill: "#A6CFE4", edge: "#79B3CE" },
    waterway: "#88BBD4",
    roads: [
      null,
      { fill: "#F2C070", case: "#C08E38", w: 4.2 },
      { fill: "#F7DC9C", case: "#C0A45C", w: 3.6 },
      { fill: "#FDFBF4", case: "#B5AF9C", w: 3.0 },
      { fill: "#FDFBF4", case: "#BFB9A6", w: 2.6 },
      { fill: "#FDFBF4", case: "#C9C3B0", w: 2.2 },
      { fill: "#FDFBF4", case: "#D4CEBB", w: 1.7 },
      { fill: "#9E6E42", case: null, w: 1.4 },
    ],
    pathDash: [4, 3],
    label: {
      place: "#2B2A24",
      water: "#33708F",
      poi: "#3A6B3C",
      road: "#4E493F",
      halo: "rgba(255,255,255,0.9)",
    },
    ui: { accent: "#C99740" },
  },

  /* 夜间：给设备 outdoor 的深色盘一个白天的对照；HTML 里只用于检查结构。 */
  night: {
    id: "night",
    title: "夜间",
    bg: "#14161A",
    land: {
      1: "#1A1D22",
      2: "#1E1A1E",
      3: "#191C20",
      4: "#232019",
      5: "#17251C",
    },
    forest: "#16241A",
    water: { fill: "#16303F", edge: "#20465A" },
    waterway: "#2A556C",
    roads: [
      null,
      { fill: "#C08A3A", case: "#6E4E1E", w: 4.2 },
      { fill: "#9C7B3C", case: "#5E4A22", w: 3.6 },
      { fill: "#4A5058", case: "#262A30", w: 3.0 },
      { fill: "#434950", case: "#24282D", w: 2.6 },
      { fill: "#3C4249", case: "#21252A", w: 2.2 },
      { fill: "#343A41", case: "#1E2227", w: 1.7 },
      { fill: "#5A6B52", case: null, w: 1.4 },
    ],
    pathDash: [4, 4],
    label: {
      place: "#E6EAF0",
      water: "#7FB6D8",
      poi: "#8FC79A",
      road: "#C3C9D2",
      halo: "rgba(12,14,18,0.9)",
    },
    ui: { accent: "#C08A3A" },
  },
};

const VMAP_STYLE_ORDER = ["paper", "bright", "terrain", "night"];

/* 缩放 → 线宽倍率（设备 zoom 14 = 1.0）。指数 0.5 让放大两档才翻倍，
 * 和"放大后路网相对变细"的观感一致。 */
function vmapZoomWidthScale(zoom) {
  return Math.min(2.4, Math.max(0.62, Math.pow(2, (zoom - 14) * 0.5)));
}

/* 每级道路从哪个 zoom 开始画：缩小只留主干，放大才出小路。
 * 注意 7（path/track/cycle）比 6（service）先出 —— 这是骑行地图，
 * 步道/绿道比小区内部路更重要。设备固定 z14，所以两者都在 z14 之后。 */
const VMAP_ROAD_MIN_ZOOM = [0, 6, 9, 11, 12.5, 13.5, 14.5, 13.5];

function vmapRoadVisible(cls, zoom) {
  return zoom >= (VMAP_ROAD_MIN_ZOOM[cls] || 0);
}

/* 用地色块在低 zoom 会糊成一片。商务/工业这两类在 z14 就是一堆灰块，
 * 对骑行没用 —— 放到 z15 才出；住/校/园保留，它们是"这是哪儿"的线索。 */
function vmapLandVisible(attr, zoom) {
  if (attr === 5) return zoom >= 11;
  if (attr === 1 || attr === 4) return zoom >= 12.5;
  if (attr === 2 || attr === 3) return zoom >= 15;
  return zoom >= 12.5;
}

const VmapPainter = (function () {
  const FONT_STACK = 'system-ui, "PingFang SC", "Noto Sans SC", "Microsoft YaHei", sans-serif';
  const LABEL_HALO_W = 3;

  /* 文字宽度缓存：同一串路名每帧要量好几次。 */
  const measureCache = new Map();
  function textWidth(ctx, text, font) {
    const key = font + "|" + text;
    let w = measureCache.get(key);
    if (w === undefined) {
      const prev = ctx.font;
      ctx.font = font;
      w = ctx.measureText(text).width;
      ctx.font = prev;
      if (measureCache.size > 4000) measureCache.clear();
      measureCache.set(key, w);
    }
    return w;
  }

  function tracePolyline(ctx, pts, closed) {
    ctx.beginPath();
    ctx.moveTo(pts[0][0], pts[0][1]);
    for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0], pts[i][1]);
    if (closed) ctx.closePath();
  }

  function fillPolys(ctx, feats, color) {
    if (!color || !feats || !feats.length) return;
    ctx.fillStyle = color;
    for (const f of feats) {
      const pts = f.spts;
      if (!pts || pts.length < 3) continue;
      tracePolyline(ctx, pts, true);
      ctx.fill();
    }
  }

  /* 水面：填色 + 一圈稍深的边，湖岸立刻清楚（设备端同样画两遍）。 */
  function fillWater(ctx, feats, st) {
    if (!feats || !feats.length) return;
    ctx.fillStyle = st.fill;
    for (const f of feats) {
      const pts = f.spts;
      if (!pts || pts.length < 3) continue;
      tracePolyline(ctx, pts, true);
      ctx.fill();
    }
    if (!st.edge) return;
    ctx.strokeStyle = st.edge;
    ctx.lineWidth = 1;
    ctx.lineJoin = "round";
    for (const f of feats) {
      const pts = f.spts;
      if (!pts || pts.length < 3) continue;
      tracePolyline(ctx, pts, true);
      ctx.stroke();
    }
  }

  function strokeLines(ctx, feats, color, width, dash) {
    if (!color || !feats || !feats.length) return;
    ctx.strokeStyle = color;
    ctx.lineWidth = width;
    ctx.lineCap = "round";
    ctx.lineJoin = "round";
    ctx.setLineDash(dash || []);
    for (const f of feats) {
      const pts = f.spts;
      if (!pts || pts.length < 2) continue;
      tracePolyline(ctx, pts, (f.flags & 1) !== 0);
      ctx.stroke();
    }
    ctx.setLineDash([]);
  }

  /* 沿路取候选点：优先长直段。返回 {x,y,ang,len} 或 null。 */
  function bestLabelAnchor(pts, needLen) {
    if (!pts || pts.length < 2) return null;
    let best = null;
    for (let i = 0; i < pts.length - 1; i++) {
      const ax = pts[i][0], ay = pts[i][1];
      const bx = pts[i + 1][0], by = pts[i + 1][1];
      const dx = bx - ax, dy = by - ay;
      const len = Math.hypot(dx, dy);
      if (len < needLen) continue;
      let ang = Math.atan2(dy, dx);
      if (ang > Math.PI / 2) ang -= Math.PI;
      if (ang < -Math.PI / 2) ang += Math.PI;
      /* 长段优先；同长时角度越平越好（读起来顺） */
      const score = len - Math.abs(ang) * 4;
      if (!best || score > best.score) {
        best = {
          x: (ax + bx) / 2,
          y: (ay + by) / 2,
          ang,
          len,
          score,
        };
      }
    }
    return best;
  }

  /* 旋转文字的保守包围盒（AABB），用于碰撞检测。 */
  function textBox(x, y, w, h, ang) {
    const c = Math.abs(Math.cos(ang));
    const s = Math.abs(Math.sin(ang));
    const bw = w * c + h * s;
    const bh = w * s + h * c;
    return { x0: x - bw / 2, y0: y - bh / 2, x1: x + bw / 2, y1: y + bh / 2 };
  }

  function boxesHit(a, b, pad) {
    return !(a.x1 + pad < b.x0 || b.x1 + pad < a.x0
      || a.y1 + pad < b.y0 || b.y1 + pad < a.y0);
  }

  function drawLabel(ctx, it, box) {
    ctx.save();
    ctx.translate(it.x, it.y);
    if (it.ang) ctx.rotate(it.ang);
    ctx.font = it.font;
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    if (it.halo) {
      ctx.lineWidth = it.haloW || LABEL_HALO_W;
      ctx.strokeStyle = it.halo;
      ctx.lineJoin = "round";
      ctx.strokeText(it.text, 0, 0);
    }
    ctx.fillStyle = it.color;
    ctx.fillText(it.text, 0, 0);
    ctx.restore();
    return box;
  }

  function labelFont(size, weight, italic) {
    return (italic ? "italic " : "") + (weight ? weight + " " : "")
      + size.toFixed(1) + "px " + FONT_STACK;
  }

  /* 统一排标签：先水/地名，再按等级排路名，最后 POI。
   * 所有标签都过一遍 AABB 碰撞；同名路只留一个。 */
  function placeLabels(ctx, scene, opts) {
    const st = opts.style;
    const lab = st.label;
    const size = opts.labelScale || 1;
    const W = opts.width;
    const H = opts.height;
    const placed = [];
    const out = { drawn: 0, skipped: 0 };

    function tryPlace(it, pad) {
      const tw = textWidth(ctx, it.text, it.font);
      const th = it.size * 1.1;
      if (it.x < -tw || it.x > W + tw || it.y < -th || it.y > H + th) {
        out.skipped++;
        return false;
      }
      const box = textBox(it.x, it.y, tw, th, it.ang || 0);
      /* 贴边时把文字拉回视口内（和设备的 clamp 一致） */
      const dx = box.x0 < 2 ? 2 - box.x0 : (box.x1 > W - 2 ? W - 2 - box.x1 : 0);
      const dy = box.y0 < 2 ? 2 - box.y0 : (box.y1 > H - 2 ? H - 2 - box.y1 : 0);
      if (dx) { it.x += dx; box.x0 += dx; box.x1 += dx; }
      if (dy) { it.y += dy; box.y0 += dy; box.y1 += dy; }
      for (const p of placed) {
        if (boxesHit(box, p, pad)) {
          out.skipped++;
          return false;
        }
      }
      placed.push(box);
      drawLabel(ctx, it, box);
      out.drawn++;
      return true;
    }

    /* 1) 点标注（水名 / 公园名 / 地名）：prio 大的先占位 */
    const points = [];
    for (const lb of scene.labels || []) {
      if (!lb.name) continue;
      if (!opts.showLabels) continue;
      const kind = lb.kind;
      let color = lab.poi;
      let font = labelFont(9.5 * size, null, false);
      let prio = lb.prio;
      if (kind === 2) {
        color = lab.water;
        font = labelFont(10.5 * size, null, true);
        prio += 40;
      } else if (kind === 1) {
        color = lab.place;
        font = labelFont(12.5 * size, "600", false);
        prio += 90;
      } else if (kind === 3) {
        continue;                       /* 路名走 2) 的沿路排版 */
      } else {
        color = lab.poi;
        font = labelFont(9.5 * size, null, false);
      }
      points.push({
        text: lb.name, x: lb.sx, y: lb.sy, ang: 0, prio,
        color, font, size: 9.5 * size, halo: lab.halo,
        haloW: kind === 1 ? 4 : LABEL_HALO_W,
      });
    }
    points.sort((a, b) => b.prio - a.prio);
    for (const it of points) tryPlace(it, 3);

    /* 2) 路名：沿路方向、按等级先后。
     * 同一个名字只出一次 —— 数据是按瓦片切的，一条路在相邻瓦片里各有一份，
     * 不加这条约束就会像旧版一样把"扬子大道"在屏上印四遍。每个名字取
     * 离屏幕中心最近的那一份，保证标在看得见的地方。 */
    if (!opts.showLabels || !opts.showRoadNames) return out;
    const byName = new Map();
    for (const f of scene.road || []) {
      if (!f.name || !f.spts || f.spts.length < 2) continue;
      if (!vmapRoadVisible(f.attr, opts.zoom)) continue;
      const font = labelFont((f.attr <= 2 ? 9.5 : 9) * size, null, false);
      const tw = textWidth(ctx, f.name, font);
      const anchor = bestLabelAnchor(f.spts, tw + 6);
      if (!anchor) { out.skipped++; continue; }
      const d = Math.hypot(anchor.x - W / 2, anchor.y - H / 2);
      const cur = byName.get(f.name);
      if (!cur || d < cur.d) {
        byName.set(f.name, {
          text: f.name, x: anchor.x, y: anchor.y, ang: anchor.ang,
          color: lab.road, font, size: 9 * size, halo: lab.halo,
          cls: f.attr, d,
        });
      }
    }
    const picks = [...byName.values()].sort((a, b) => (a.cls - b.cls) || (a.d - b.d));
    for (const it of picks) tryPlace(it, 2);
    return out;
  }

  /**
   * 画一整帧。
   * @param ctx 2D 上下文（已按 dpr 缩放）
   * @param scene {land,forest,water,waterway,road,labels}
   * @param opts  {style, zoom, width, height, showLand, showLabels,
   *               showRoadNames, widthScale, labelScale, device}
   */
  function paint(ctx, scene, opts) {
    const st = opts.style;
    const zoom = opts.zoom;
    const ws = (opts.widthScale || 1) * vmapZoomWidthScale(zoom);
    /* 壳要比芯厚出"看得见的一圈"：太薄等于没有，太厚会把小路糊死。 */
    const caseExtra = Math.max(1.1, 1.9 * Math.min(1.7, ws));

    ctx.fillStyle = st.bg;
    ctx.fillRect(0, 0, opts.width, opts.height);

    if (opts.showLand) {
      for (const attr of [1, 2, 3, 4, 5]) {
        if (!vmapLandVisible(attr, zoom)) continue;
        fillPolys(ctx, (scene.land || []).filter((f) => f.attr === attr), st.land[attr]);
      }
      fillPolys(ctx, scene.forest, st.forest);
    }
    fillWater(ctx, scene.water, st.water);
    strokeLines(ctx, scene.waterway, st.waterway,
      Math.max(1, 1.4 * Math.min(1.5, ws)), null);

    /* 道路：从低等级到高等级，每条 = 壳 + 芯；高等级后画，路口自然压住低等级。 */
    for (let cls = 7; cls >= 1; cls--) {
      const rs = st.roads[cls];
      if (!rs || !vmapRoadVisible(cls, zoom)) continue;
      const feats = (scene.road || []).filter((f) => f.attr === cls);
      if (!feats.length) continue;
      const w = rs.w * ws;
      const dash = cls === 7 && st.pathDash
        ? [st.pathDash[0] * ws, st.pathDash[1] * ws] : null;
      if (rs.case && w > 1.2) {
        strokeLines(ctx, feats, rs.case, w + caseExtra, null);
      }
      strokeLines(ctx, feats, rs.fill, w, dash);
    }

    if (opts.showLabels) placeLabels(ctx, scene, opts);
  }

  return { paint, placeLabels, bestLabelAnchor, textWidth, labelFont };
})();
