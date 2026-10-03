"use client";

import { useEffect, useRef } from "react";
import { LineChart } from "echarts/charts";
import {
  GridComponent,
  LegendComponent,
  TooltipComponent,
} from "echarts/components";
import * as echarts from "echarts/core";
import { CanvasRenderer } from "echarts/renderers";
import type { TelemetryPoint } from "../src/protocol/schema";

echarts.use([
  LineChart,
  GridComponent,
  LegendComponent,
  TooltipComponent,
  CanvasRenderer,
]);

type Thresholds = {
  tempHigh?: number;
  tempLow?: number;
  humidityHigh?: number;
  humidityLow?: number;
};

type Props = {
  points: TelemetryPoint[];
  thresholds?: Thresholds;
};

function bounds(values: number[], fallback: [number, number]) {
  if (!values.length) return { min: fallback[0], max: fallback[1] };
  const min = Math.min(...values);
  const max = Math.max(...values);
  const pad = Math.max(1, (max - min) * 0.12);
  return { min: Math.floor(min - pad), max: Math.ceil(max + pad) };
}

export function TelemetryChart({ points, thresholds }: Props) {
  const elementRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!elementRef.current) return;
    const chart = echarts.init(elementRef.current, undefined, { renderer: "canvas" });
    const isDark = document.documentElement.getAttribute("data-theme") === "dark";
    const axisColor = isDark ? "#9CA3AF" : "#71808c";
    const splitColor = isDark ? "#2A3441" : "#edf1f3";
    const tempColor = isDark ? "#14B8A6" : "#087e73";
    const humColor = isDark ? "#60A5FA" : "#2b6cb0";

    const labels = points.map((point) =>
      new Intl.DateTimeFormat("zh-CN", {
        month: "2-digit",
        day: "2-digit",
        hour: "2-digit",
        minute: "2-digit",
        hour12: false,
      }).format(point.timestamp_epoch * 1000),
    );

    const temps = points.map((p) => p.temperature_avg).filter((v): v is number => v != null);
    const hums = points.map((p) => p.humidity_avg).filter((v): v is number => v != null);
    const tempHigh = thresholds?.tempHigh ?? 30;
    const tempLow = thresholds?.tempLow;
    const humidityHigh = thresholds?.humidityHigh ?? 70;
    const humidityLow = thresholds?.humidityLow;

    const temp = bounds([...temps, tempHigh, ...(tempLow != null ? [tempLow] : [])], [24, 32]);
    const hum = bounds([...hums, humidityHigh, ...(humidityLow != null ? [humidityLow] : [])], [50, 75]);

    const markLines: Array<Record<string, unknown>> = [
      { yAxis: tempHigh, lineStyle: { color: "#ce5b43", type: "dashed", width: 1 }, label: { formatter: `高温 ${tempHigh}°C`, color: "#9f4433" } },
    ];
    if (tempLow != null) {
      markLines.push({ yAxis: tempLow, lineStyle: { color: "#3b82f6", type: "dashed", width: 1 }, label: { formatter: `低温 ${tempLow}°C`, color: "#1e40af" } });
    }

    chart.setOption({
      animationDuration: 350,
      color: [tempColor, humColor],
      grid: { left: 40, right: 46, top: 48, bottom: 30 },
      legend: { top: 4, right: 0, itemWidth: 16, textStyle: { color: axisColor, fontSize: 11 } },
      tooltip: {
        trigger: "axis",
        valueFormatter: (value: number | string) =>
          typeof value === "number" ? value.toFixed(2) : value,
      },
      xAxis: {
        type: "category",
        data: labels,
        boundaryGap: false,
        axisLine: { lineStyle: { color: splitColor } },
        axisLabel: { color: axisColor, fontSize: 10, interval: 5 },
      },
      yAxis: [
        {
          type: "value",
          min: temp.min,
          max: temp.max,
          axisLabel: { formatter: "{value} °C", color: axisColor },
          splitLine: { lineStyle: { color: splitColor } },
        },
        {
          type: "value",
          min: hum.min,
          max: hum.max,
          axisLabel: { formatter: "{value}%", color: axisColor },
          splitLine: { show: false },
        },
      ],
      series: [
        {
          name: "温度",
          type: "line",
          smooth: 0.28,
          symbol: "none",
          lineStyle: { width: 2.4 },
          areaStyle: { color: isDark ? "rgba(20,184,166,.12)" : "rgba(8,126,115,.08)" },
          data: points.map((point) => point.temperature_avg),
          markLine: { silent: true, symbol: "none", data: markLines },
        },
        {
          name: "湿度",
          type: "line",
          yAxisIndex: 1,
          smooth: 0.28,
          symbol: "none",
          lineStyle: { width: 2 },
          data: points.map((point) => point.humidity_avg),
        },
      ],
    });

    const resize = () => chart.resize();
    window.addEventListener("resize", resize);
    return () => {
      window.removeEventListener("resize", resize);
      chart.dispose();
    };
  }, [points, thresholds]);

  return (
    <div
      ref={elementRef}
      className="telemetry-chart"
      role="img"
      aria-label="温湿度趋势曲线"
    />
  );
}
