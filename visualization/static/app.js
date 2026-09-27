(() => {
  "use strict";

  const CHART_MAX_HZ = 1000; // All configured target bands sit well under this.
  const CHART_MIN_DBFS = -120;
  const CHART_MAX_DBFS = 0;
  const CHART_W = 680;
  const CHART_H = 220;
  const MARGIN = { top: 12, right: 8, bottom: 22, left: 30 };
  const MAX_ANGLE_DEG = 180;

  // Fixed 2x2 grid positions, matching the physical block's pocket layout
  // (top-left, top-right, bottom-left, bottom-right). Servo-id-to-corner
  // mapping is a placeholder until the real assembly wiring is confirmed.
  const CELL_ORDER = [1, 2, 3, 4];

  const assembly = document.getElementById("assembly");
  const connectionStatus = document.getElementById("connection-status");
  const rmsReadout = document.getElementById("rms-readout");
  const svg = document.getElementById("spectrum-chart");
  const tooltip = document.getElementById("chart-tooltip");

  const cavityCells = new Map();
  let latestSpectrum = null;

  function cssVar(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }

  // Depth is encoded purely as lightness of the assembly's own lavender
  // material color — shallow stays pale, deep stays a touch more saturated,
  // but always within a narrow, light band (never a deep/dark purple).
  function depthLightness(fraction) {
    const clamped = Math.max(0, Math.min(1, fraction));
    return 80 - clamped * 20; // 80% (shallow) -> 60% (deep)
  }

  function ensureCavityCell(id) {
    if (cavityCells.has(id)) {
      return cavityCells.get(id);
    }
    const cell = document.createElement("div");
    cell.className = "cell";
    const swatch = document.createElement("div");
    swatch.className = "cell__swatch";
    swatch.innerHTML = `
      <div class="cell__id">C${id}</div>
      <div class="cell__depth" data-field="depth">— <span class="cell__depth-unit" data-field="unit"></span></div>
      <div class="cell__meta">
        <span data-field="target">—</span>
        <span data-field="level">—</span>
      </div>
    `;
    cell.appendChild(swatch);
    assembly.appendChild(cell);
    const refs = {
      swatch,
      depth: swatch.querySelector('[data-field="depth"]').firstChild,
      unit: swatch.querySelector('[data-field="unit"]'),
      target: swatch.querySelector('[data-field="target"]'),
      level: swatch.querySelector('[data-field="level"]'),
    };
    cavityCells.set(id, refs);
    return refs;
  }

  function updateCavityCell(cavity) {
    const refs = ensureCavityCell(cavity.id);

    let fraction;
    if (cavity.depth_mm != null) {
      const maxDepthMm = cavity.max_depth_mm || cavity.depth_mm || 1;
      fraction = cavity.depth_mm / maxDepthMm;
      refs.depth.textContent = cavity.depth_mm.toFixed(1);
      refs.unit.textContent = "mm";
    } else {
      fraction = cavity.angle_deg / MAX_ANGLE_DEG;
      refs.depth.textContent = cavity.angle_deg;
      refs.unit.textContent = "°";
    }

    const hue = cssVar("--cell-hue");
    const sat = cssVar("--cell-sat");
    refs.swatch.style.setProperty("--cell-fill", `hsl(${hue} ${sat} ${depthLightness(fraction)}%)`);

    refs.target.textContent = cavity.target_hz != null ? `${cavity.target_hz.toFixed(0)}hz` : "—";
    refs.level.textContent = cavity.level_dbfs != null ? `${cavity.level_dbfs.toFixed(0)}db` : "—";
  }

  function renderAssembly(frame) {
    const byId = new Map(frame.cavities.map((cavity) => [cavity.id, cavity]));
    CELL_ORDER.forEach((id) => {
      const cavity = byId.get(id);
      if (cavity) {
        updateCavityCell(cavity);
      }
    });
  }

  function updateConnectionStatus(connected) {
    connectionStatus.classList.toggle("status--connected", connected);
    connectionStatus.classList.toggle("status--disconnected", !connected);
    connectionStatus.querySelector(".status-text").textContent = connected
      ? "esp32 connected"
      : "esp32 unreachable";
  }

  function xToPixel(freqHz) {
    const plotWidth = CHART_W - MARGIN.left - MARGIN.right;
    return MARGIN.left + (freqHz / CHART_MAX_HZ) * plotWidth;
  }

  function yToPixel(dbfs) {
    const plotHeight = CHART_H - MARGIN.top - MARGIN.bottom;
    const clamped = Math.max(CHART_MIN_DBFS, Math.min(CHART_MAX_DBFS, dbfs));
    const fraction = (clamped - CHART_MIN_DBFS) / (CHART_MAX_DBFS - CHART_MIN_DBFS);
    return MARGIN.top + (1 - fraction) * plotHeight;
  }

  function svgEl(tag, attrs) {
    const el = document.createElementNS("http://www.w3.org/2000/svg", tag);
    for (const [key, value] of Object.entries(attrs)) {
      el.setAttribute(key, value);
    }
    return el;
  }

  function renderChart(frame) {
    svg.innerHTML = "";
    svg.setAttribute("viewBox", `0 0 ${CHART_W} ${CHART_H}`);

    const inkMuted = cssVar("--ink-muted");
    const inkFaint = cssVar("--ink-faint");
    const accent = cssVar("--accent");

    svg.appendChild(
      svgEl("line", {
        x1: MARGIN.left,
        x2: CHART_W - MARGIN.right,
        y1: CHART_H - MARGIN.bottom,
        y2: CHART_H - MARGIN.bottom,
        stroke: inkFaint,
        "stroke-width": 1,
      })
    );

    [0, 500, 1000].forEach((hz) => {
      svg.appendChild(
        svgEl("text", {
          x: xToPixel(hz),
          y: CHART_H - MARGIN.bottom + 14,
          "text-anchor": hz === 0 ? "start" : hz === CHART_MAX_HZ ? "end" : "middle",
          "font-family": "IBM Plex Mono, monospace",
          "font-size": 9.5,
          fill: inkMuted,
        })
      ).textContent = `${hz}hz`;
    });
    [CHART_MAX_DBFS, CHART_MIN_DBFS].forEach((dbfs) => {
      svg.appendChild(
        svgEl("text", {
          x: MARGIN.left - 6,
          y: yToPixel(dbfs) + (dbfs === CHART_MAX_DBFS ? 8 : 0),
          "text-anchor": "end",
          "font-family": "IBM Plex Mono, monospace",
          "font-size": 9.5,
          fill: inkMuted,
        })
      ).textContent = `${dbfs}`;
    });

    if (frame.spectrum && frame.spectrum.dbfs && frame.spectrum.dbfs.length > 0) {
      const binWidthHz = frame.spectrum.bin_width_hz;
      const points = [];
      frame.spectrum.dbfs.forEach((dbfs, bin) => {
        const freqHz = bin * binWidthHz;
        if (freqHz > CHART_MAX_HZ) {
          return;
        }
        points.push(`${xToPixel(freqHz)},${yToPixel(dbfs)}`);
      });
      if (points.length > 1) {
        svg.appendChild(
          svgEl("polyline", {
            points: points.join(" "),
            fill: "none",
            stroke: inkMuted,
            "stroke-width": 1.25,
            "stroke-linejoin": "round",
          })
        );
      }
    }

    frame.cavities.forEach((cavity) => {
      if (cavity.target_hz == null || cavity.target_hz > CHART_MAX_HZ) {
        return;
      }
      const x = xToPixel(cavity.target_hz);
      svg.appendChild(
        svgEl("line", {
          x1: x,
          x2: x,
          y1: MARGIN.top,
          y2: CHART_H - MARGIN.bottom,
          stroke: accent,
          "stroke-width": 1,
          "stroke-dasharray": "3,3",
          opacity: 0.85,
        })
      );
      svg.appendChild(
        svgEl("text", {
          x: x + 3,
          y: MARGIN.top + 9,
          "font-family": "IBM Plex Mono, monospace",
          "font-size": 9.5,
          fill: accent,
        })
      ).textContent = `c${cavity.id}`;
    });
  }

  function handlePointerMove(event) {
    if (!latestSpectrum || !latestSpectrum.dbfs || latestSpectrum.dbfs.length === 0) {
      return;
    }
    const rect = svg.getBoundingClientRect();
    const fractionX = (event.clientX - rect.left) / rect.width;
    const viewBoxX = fractionX * CHART_W;
    const plotWidth = CHART_W - MARGIN.left - MARGIN.right;
    const freqFraction = (viewBoxX - MARGIN.left) / plotWidth;
    if (freqFraction < 0 || freqFraction > 1) {
      tooltip.hidden = true;
      return;
    }
    const freqHz = freqFraction * CHART_MAX_HZ;
    const binWidthHz = latestSpectrum.bin_width_hz;
    const bin = Math.round(freqHz / binWidthHz);
    const dbfs = latestSpectrum.dbfs[bin];
    if (dbfs == null) {
      tooltip.hidden = true;
      return;
    }
    tooltip.hidden = false;
    tooltip.style.left = `${event.clientX - rect.left}px`;
    tooltip.style.top = `${event.clientY - rect.top}px`;
    tooltip.textContent = `${(bin * binWidthHz).toFixed(0)}hz  ${dbfs.toFixed(1)}db`;
  }

  svg.addEventListener("mousemove", handlePointerMove);
  svg.addEventListener("mouseleave", () => {
    tooltip.hidden = true;
  });

  function applyFrame(frame) {
    updateConnectionStatus(frame.connected);
    renderAssembly(frame);
    rmsReadout.textContent = frame.rms_dbfs != null ? `rms ${frame.rms_dbfs.toFixed(0)}db` : "rms —";
    latestSpectrum = frame.spectrum;
    renderChart(frame);
  }

  function connect() {
    const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
    const socket = new WebSocket(`${protocol}//${window.location.host}/ws`);

    socket.addEventListener("message", (event) => {
      try {
        applyFrame(JSON.parse(event.data));
      } catch (error) {
        console.error("Failed to parse dashboard frame", error);
      }
    });

    socket.addEventListener("close", () => {
      updateConnectionStatus(false);
      setTimeout(connect, 1000);
    });

    socket.addEventListener("error", () => {
      socket.close();
    });
  }

  connect();
})();
