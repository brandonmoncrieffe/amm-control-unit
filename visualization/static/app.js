(() => {
  "use strict";

  const CAVITY_COLORS = {
    1: "var(--series-1)",
    2: "var(--series-2)",
    3: "var(--series-3)",
    4: "var(--series-4)",
  };
  const CAVITY_COLOR_HEX = {
    // Used for SVG stroke/fill, which cannot resolve CSS var() the same way
    // text/background properties can in every browser reliably for SVG attrs.
    1: getComputedColor("--series-1", "#2a78d6"),
    2: getComputedColor("--series-2", "#eb6834"),
    3: getComputedColor("--series-3", "#1baf7a"),
    4: getComputedColor("--series-4", "#eda100"),
  };

  const CHART_MAX_HZ = 1000; // All configured target bands sit well under this.
  const CHART_MIN_DBFS = -120;
  const CHART_MAX_DBFS = 0;
  const MARGIN = { top: 12, right: 16, bottom: 28, left: 44 };
  const VIEWBOX_WIDTH = 960;
  const VIEWBOX_HEIGHT = 360;

  const cavityGrid = document.getElementById("cavity-grid");
  const connectionStatus = document.getElementById("connection-status");
  const rmsReadout = document.getElementById("rms-readout");
  const chartLegend = document.getElementById("chart-legend");
  const svg = document.getElementById("spectrum-chart");
  const tooltip = document.getElementById("chart-tooltip");

  const cavityCards = new Map();
  let latestSpectrum = null;

  function getComputedColor(varName, fallback) {
    const value = getComputedStyle(document.querySelector(".viz-root") || document.body)
      .getPropertyValue(varName)
      .trim();
    return value || fallback;
  }

  function ensureCavityCard(id) {
    if (cavityCards.has(id)) {
      return cavityCards.get(id);
    }
    const card = document.createElement("article");
    card.className = "cavity-card";
    card.style.setProperty("--cavity-color", CAVITY_COLORS[id] || "var(--series-1)");
    card.innerHTML = `
      <div class="cavity-card__title">
        <span class="cavity-card__swatch"></span>
        <span>Cavity ${id}</span>
      </div>
      <div class="cavity-card__metric"><span>Depth</span><strong data-field="depth">—</strong></div>
      <div class="cavity-card__metric"><span>Blocking</span><strong data-field="target">—</strong></div>
      <div class="cavity-card__metric"><span>Level</span><strong data-field="level">—</strong></div>
      <div class="cavity-card__bar-track"><div class="cavity-card__bar-fill"></div></div>
      <div class="cavity-card__bar-caption" data-field="caption">Commanded angle</div>
    `;
    cavityGrid.appendChild(card);
    const refs = {
      root: card,
      depth: card.querySelector('[data-field="depth"]'),
      target: card.querySelector('[data-field="target"]'),
      level: card.querySelector('[data-field="level"]'),
      caption: card.querySelector('[data-field="caption"]'),
      barFill: card.querySelector(".cavity-card__bar-fill"),
    };
    cavityCards.set(id, refs);
    return refs;
  }

  function updateCavityCard(cavity) {
    const refs = ensureCavityCard(cavity.id);

    if (cavity.depth_mm != null) {
      refs.depth.textContent = `${cavity.depth_mm.toFixed(1)} mm`;
      refs.caption.textContent = "Calibrated piston depth";
      const maxDepthMm = cavity.max_depth_mm || cavity.depth_mm || 1;
      const fraction = Math.max(0, Math.min(1, cavity.depth_mm / maxDepthMm));
      refs.barFill.style.width = `${(fraction * 100).toFixed(0)}%`;
    } else {
      refs.depth.textContent = `${cavity.angle_deg}°`;
      refs.caption.textContent = "Commanded angle (not calibrated)";
      refs.barFill.style.width = `${(cavity.angle_deg / 180) * 100}%`;
    }

    refs.target.textContent = cavity.target_hz != null ? `${cavity.target_hz.toFixed(0)} Hz` : "—";
    refs.level.textContent = cavity.level_dbfs != null ? `${cavity.level_dbfs.toFixed(1)} dBFS` : "—";
  }

  function updateConnectionStatus(connected) {
    connectionStatus.classList.toggle("status-pill--connected", connected);
    connectionStatus.classList.toggle("status-pill--disconnected", !connected);
    connectionStatus.querySelector(".status-text").textContent = connected
      ? "ESP32 connected"
      : "ESP32 unreachable";
  }

  function xToPixel(freqHz) {
    const plotWidth = VIEWBOX_WIDTH - MARGIN.left - MARGIN.right;
    return MARGIN.left + (freqHz / CHART_MAX_HZ) * plotWidth;
  }

  function yToPixel(dbfs) {
    const plotHeight = VIEWBOX_HEIGHT - MARGIN.top - MARGIN.bottom;
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
    svg.setAttribute("viewBox", `0 0 ${VIEWBOX_WIDTH} ${VIEWBOX_HEIGHT}`);

    const gridColor = getComputedColor("--gridline", "#e1e0d9");
    const mutedColor = getComputedColor("--text-muted", "#898781");
    const baselineColor = getComputedColor("--baseline", "#c3c2b7");

    // Horizontal gridlines every 20 dBFS, with axis labels (recessive ink).
    for (let dbfs = CHART_MAX_DBFS; dbfs >= CHART_MIN_DBFS; dbfs -= 20) {
      const y = yToPixel(dbfs);
      svg.appendChild(
        svgEl("line", {
          x1: MARGIN.left,
          x2: VIEWBOX_WIDTH - MARGIN.right,
          y1: y,
          y2: y,
          stroke: gridColor,
          "stroke-width": 1,
        })
      );
      svg.appendChild(
        svgEl("text", {
          x: MARGIN.left - 8,
          y: y + 3,
          "text-anchor": "end",
          "font-size": 10,
          fill: mutedColor,
        })
      ).textContent = `${dbfs}`;
    }

    // Vertical gridlines every 200 Hz.
    for (let hz = 0; hz <= CHART_MAX_HZ; hz += 200) {
      const x = xToPixel(hz);
      svg.appendChild(
        svgEl("line", {
          x1: x,
          x2: x,
          y1: MARGIN.top,
          y2: VIEWBOX_HEIGHT - MARGIN.bottom,
          stroke: gridColor,
          "stroke-width": 1,
        })
      );
      svg.appendChild(
        svgEl("text", {
          x,
          y: VIEWBOX_HEIGHT - MARGIN.bottom + 16,
          "text-anchor": "middle",
          "font-size": 10,
          fill: mutedColor,
        })
      ).textContent = `${hz}`;
    }

    // Baseline axes.
    svg.appendChild(
      svgEl("line", {
        x1: MARGIN.left,
        x2: MARGIN.left,
        y1: MARGIN.top,
        y2: VIEWBOX_HEIGHT - MARGIN.bottom,
        stroke: baselineColor,
        "stroke-width": 1,
      })
    );
    svg.appendChild(
      svgEl("line", {
        x1: MARGIN.left,
        x2: VIEWBOX_WIDTH - MARGIN.right,
        y1: VIEWBOX_HEIGHT - MARGIN.bottom,
        y2: VIEWBOX_HEIGHT - MARGIN.bottom,
        stroke: baselineColor,
        "stroke-width": 1,
      })
    );

    // Incoming spectrum line.
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
            stroke: getComputedColor("--text-secondary", "#52514e"),
            "stroke-width": 2,
            "stroke-linejoin": "round",
          })
        );
      }
    }

    // One dashed marker per cavity at its blocked/target frequency.
    frame.cavities.forEach((cavity) => {
      if (cavity.target_hz == null || cavity.target_hz > CHART_MAX_HZ) {
        return;
      }
      const x = xToPixel(cavity.target_hz);
      const color = CAVITY_COLOR_HEX[cavity.id] || CAVITY_COLOR_HEX[1];
      svg.appendChild(
        svgEl("line", {
          x1: x,
          x2: x,
          y1: MARGIN.top,
          y2: VIEWBOX_HEIGHT - MARGIN.bottom,
          stroke: color,
          "stroke-width": 2,
          "stroke-dasharray": "5,4",
        })
      );
      svg.appendChild(
        svgEl("text", {
          x: x + 4,
          y: MARGIN.top + 10,
          "font-size": 10,
          fill: color,
          "font-weight": 600,
        })
      ).textContent = `${cavity.target_hz.toFixed(0)} Hz`;
    });
  }

  function renderLegend(frame) {
    chartLegend.innerHTML = "";
    const spectrumItem = document.createElement("span");
    spectrumItem.className = "chart-legend__item";
    spectrumItem.style.color = getComputedColor("--text-secondary", "#52514e");
    spectrumItem.innerHTML = `<span class="chart-legend__swatch"></span> Incoming spectrum`;
    chartLegend.appendChild(spectrumItem);

    frame.cavities.forEach((cavity) => {
      const item = document.createElement("span");
      item.className = "chart-legend__item";
      item.style.color = CAVITY_COLOR_HEX[cavity.id] || CAVITY_COLOR_HEX[1];
      const targetLabel = cavity.target_hz != null ? `${cavity.target_hz.toFixed(0)} Hz` : "unset";
      item.innerHTML = `<span class="chart-legend__swatch"></span> Cavity ${cavity.id} target (${targetLabel})`;
      chartLegend.appendChild(item);
    });
  }

  function handlePointerMove(event) {
    if (!latestSpectrum || !latestSpectrum.dbfs || latestSpectrum.dbfs.length === 0) {
      return;
    }
    const rect = svg.getBoundingClientRect();
    const fractionX = (event.clientX - rect.left) / rect.width;
    const viewBoxX = fractionX * VIEWBOX_WIDTH;
    const plotWidth = VIEWBOX_WIDTH - MARGIN.left - MARGIN.right;
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
    tooltip.textContent = `${(bin * binWidthHz).toFixed(1)} Hz — ${dbfs.toFixed(1)} dBFS`;
  }

  svg.addEventListener("mousemove", handlePointerMove);
  svg.addEventListener("mouseleave", () => {
    tooltip.hidden = true;
  });

  function applyFrame(frame) {
    updateConnectionStatus(frame.connected);
    frame.cavities.forEach(updateCavityCard);
    rmsReadout.textContent = frame.rms_dbfs != null ? `RMS: ${frame.rms_dbfs.toFixed(1)} dBFS` : "RMS: —";
    latestSpectrum = frame.spectrum;
    renderChart(frame);
    renderLegend(frame);
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
