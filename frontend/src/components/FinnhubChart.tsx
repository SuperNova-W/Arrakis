import { useEffect, useMemo, useRef, useState } from 'react'
import {
  AreaSeries,
  CandlestickSeries,
  ColorType,
  CrosshairMode,
  HistogramSeries,
  LineSeries,
  LineStyle,
  createChart,
  type IChartApi,
  type ISeriesApi,
  type MouseEventParams,
  type Time,
  type UTCTimestamp,
} from 'lightweight-charts'
import type { Candle, ChartStyle, IndicatorBundle, IndicatorKey } from '../finnhub/types'

type Props = {
  symbol: string
  candles: Candle[]
  benchmarkSymbol: string
  benchmark: Candle[]
  style: ChartStyle
  indicators: IndicatorBundle | null
  activeIndicators: Set<IndicatorKey>
}

function chartTime(time: number) {
  return time as UTCTimestamp
}

function currency(value: number) {
  return `$${value.toFixed(2)}`
}

export default function FinnhubChart({
  symbol,
  candles,
  benchmarkSymbol,
  benchmark,
  style,
  indicators,
  activeIndicators,
}: Props) {
  const container = useRef<HTMLDivElement>(null)
  const chartApi = useRef<IChartApi | null>(null)
  const [hovered, setHovered] = useState<Candle | null>(null)
  const candleMap = useMemo(() => new Map(candles.map(candle => [candle.time, candle])), [candles])

  useEffect(() => {
    if (!container.current || !candles.length) return
    const chart = createChart(container.current, {
      width: container.current.clientWidth,
      height: 570,
      layout: {
        background: { type: ColorType.Solid, color: '#ffffff' },
        textColor: '#6a7d89',
        fontFamily: 'Inter, ui-sans-serif, system-ui, sans-serif',
        fontSize: 11,
        panes: { separatorColor: '#e1e8ec', separatorHoverColor: '#c6d4da', enableResize: true },
      },
      grid: {
        vertLines: { color: '#eef2f4', style: LineStyle.Dotted },
        horzLines: { color: '#dfe7eb', style: LineStyle.Dashed },
      },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: '#78929d', style: LineStyle.Dashed, labelBackgroundColor: '#315d6d' },
        horzLine: { color: '#78929d', style: LineStyle.Dashed, labelBackgroundColor: '#315d6d' },
      },
      rightPriceScale: { borderColor: '#dce5e9', scaleMargins: { top: .08, bottom: .24 } },
      timeScale: {
        borderColor: '#dce5e9',
        timeVisible: true,
        secondsVisible: false,
        rightOffset: 2,
        barSpacing: 8,
        minBarSpacing: .6,
      },
      handleScroll: true,
      handleScale: true,
    })
    chartApi.current = chart

    let priceSeries: ISeriesApi<'Area'> | ISeriesApi<'Candlestick'>
    if (style === 'candles') {
      priceSeries = chart.addSeries(CandlestickSeries, {
        upColor: '#25826b',
        downColor: '#d45d5d',
        wickUpColor: '#25826b',
        wickDownColor: '#d45d5d',
        borderVisible: false,
        priceLineVisible: true,
        lastValueVisible: true,
      })
      priceSeries.setData(candles.map(candle => ({
        time: chartTime(candle.time),
        open: candle.open,
        high: candle.high,
        low: candle.low,
        close: candle.close,
      })))
    } else {
      priceSeries = chart.addSeries(AreaSeries, {
        lineColor: '#277d69',
        topColor: 'rgba(39, 125, 105, .28)',
        bottomColor: 'rgba(39, 125, 105, .03)',
        lineWidth: 2,
        priceLineVisible: true,
        lastValueVisible: true,
      })
      priceSeries.setData(candles.map(candle => ({ time: chartTime(candle.time), value: candle.close })))
    }

    priceSeries.createPriceLine({
      price: candles[0]!.open,
      color: '#93a5ae',
      lineWidth: 1,
      lineStyle: LineStyle.Dashed,
      axisLabelVisible: true,
      title: 'Open',
    })

    const volumeSeries = chart.addSeries(HistogramSeries, {
      priceFormat: { type: 'volume' },
      priceScaleId: 'volume',
      lastValueVisible: false,
      priceLineVisible: false,
    })
    volumeSeries.priceScale().applyOptions({ scaleMargins: { top: .78, bottom: 0 } })
    volumeSeries.setData(candles.map(candle => ({
      time: chartTime(candle.time),
      value: candle.volume,
      color: candle.close >= candle.open ? 'rgba(39,125,105,.32)' : 'rgba(212,93,93,.30)',
    })))

    const addLine = (points: Array<{ time: number; value: number }>, color: string, title: string, lineStyle = LineStyle.Solid) => {
      const series = chart.addSeries(LineSeries, {
        color,
        lineWidth: 1,
        lineStyle,
        title,
        priceLineVisible: false,
        lastValueVisible: false,
        crosshairMarkerVisible: false,
      })
      series.setData(points.map(point => ({ time: chartTime(point.time), value: point.value })))
      return series
    }

    if (indicators && activeIndicators.has('sma20')) addLine(indicators.sma20, '#3278b5', 'SMA 20')
    if (indicators && activeIndicators.has('sma50')) addLine(indicators.sma50, '#a66b22', 'SMA 50')
    if (indicators && activeIndicators.has('sma200')) addLine(indicators.sma200, '#7b5bb3', 'SMA 200')
    if (indicators && activeIndicators.has('ema20')) addLine(indicators.ema20, '#d55c91', 'EMA 20')
    if (indicators && activeIndicators.has('bollinger')) {
      addLine(indicators.bollingerUpper, '#7c92a0', 'BB upper', LineStyle.Dotted)
      addLine(indicators.bollingerLower, '#7c92a0', 'BB lower', LineStyle.Dotted)
    }

    if (benchmark.length > 1) {
      const first = benchmark[0]!.close
      const base = candles[0]!.close
      addLine(
        benchmark.map(candle => ({ time: candle.time, value: base * candle.close / first })),
        '#6375c7',
        benchmarkSymbol,
        LineStyle.Dashed,
      )
    }

    if (indicators && activeIndicators.has('rsi')) {
      const rsiSeries = chart.addSeries(LineSeries, {
        color: '#6755a5',
        lineWidth: 1,
        title: 'RSI 14',
        priceLineVisible: false,
        lastValueVisible: true,
        priceFormat: { type: 'price', precision: 1, minMove: .1 },
      }, 1)
      rsiSeries.setData(indicators.rsi.map(point => ({ time: chartTime(point.time), value: point.value })))
      rsiSeries.createPriceLine({ price: 70, color: '#c17777', lineWidth: 1, lineStyle: LineStyle.Dotted, axisLabelVisible: true, title: '70' })
      rsiSeries.createPriceLine({ price: 30, color: '#6e9c8a', lineWidth: 1, lineStyle: LineStyle.Dotted, axisLabelVisible: true, title: '30' })
    }

    if (indicators && activeIndicators.has('macd')) {
      const paneIndex = activeIndicators.has('rsi') ? 2 : 1
      const macdLine = chart.addSeries(LineSeries, { color: '#2e6e9e', lineWidth: 1, title: 'MACD', priceLineVisible: false }, paneIndex)
      const signalLine = chart.addSeries(LineSeries, { color: '#c06d38', lineWidth: 1, title: 'Signal', priceLineVisible: false }, paneIndex)
      const histogram = chart.addSeries(HistogramSeries, { priceLineVisible: false, lastValueVisible: false }, paneIndex)
      macdLine.setData(indicators.macd.map(point => ({ time: chartTime(point.time), value: point.value })))
      signalLine.setData(indicators.macdSignal.map(point => ({ time: chartTime(point.time), value: point.value })))
      histogram.setData(indicators.macdHistogram.map(point => ({ time: chartTime(point.time), value: point.value, color: point.color })))
    }

    const crosshairHandler = (parameter: MouseEventParams<Time>) => {
      if (!parameter.time) {
        setHovered(null)
        return
      }
      setHovered(candleMap.get(Number(parameter.time)) ?? null)
    }
    chart.subscribeCrosshairMove(crosshairHandler)
    chart.timeScale().fitContent()

    const resizeObserver = new ResizeObserver(entries => {
      const width = entries[0]?.contentRect.width
      if (width) chart.applyOptions({ width })
    })
    resizeObserver.observe(container.current)

    return () => {
      resizeObserver.disconnect()
      chart.unsubscribeCrosshairMove(crosshairHandler)
      chart.remove()
      chartApi.current = null
    }
  }, [activeIndicators, benchmark, benchmarkSymbol, candleMap, candles, indicators, style])

  const display = hovered ?? candles.at(-1) ?? null
  return <div className="tv-chart-shell">
    {display && <div className="chart-crosshair-readout" aria-live="polite">
      <span>{new Date(display.time * 1000).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' })}</span>
      <b>O {currency(display.open)}</b>
      <b>H {currency(display.high)}</b>
      <b>L {currency(display.low)}</b>
      <b>C {currency(display.close)}</b>
      <b>Vol {Math.round(display.volume).toLocaleString()}</b>
    </div>}
    <div ref={container} className="tv-chart" aria-label={`${symbol} Finnhub history and live OHLCV chart`}/>
  </div>
}
