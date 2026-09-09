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
        textColor: '#5c685f',
        fontFamily: 'Arial, Helvetica, sans-serif',
        fontSize: 12,
        panes: { separatorColor: '#dce2dc', separatorHoverColor: '#bacbb5', enableResize: true },
      },
      grid: {
        vertLines: { color: '#f0f3ed', style: LineStyle.Dotted },
        horzLines: { color: '#e0e7db', style: LineStyle.Dashed },
      },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: '#78929d', style: LineStyle.Dashed, labelBackgroundColor: '#102c21' },
        horzLine: { color: '#78929d', style: LineStyle.Dashed, labelBackgroundColor: '#102c21' },
      },
      rightPriceScale: { borderColor: '#dce2dc', scaleMargins: { top: .08, bottom: .24 } },
      timeScale: {
        borderColor: '#dce2dc',
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
        upColor: '#386d40',
        downColor: '#d45d5d',
        wickUpColor: '#386d40',
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
        lineColor: '#386d40',
        topColor: 'rgba(56, 109, 64, .28)',
        bottomColor: 'rgba(56, 109, 64, .03)',
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
    {display && <div className="chart-crosshair-readout">
      <span>{new Date(display.time * 1000).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' })}</span>
      <b>Open {currency(display.open)}</b>
      <b>High {currency(display.high)}</b>
      <b>Low {currency(display.low)}</b>
      <b>Close {currency(display.close)}</b>
      <b>Volume {Math.round(display.volume).toLocaleString()}</b>
    </div>}
    <div ref={container} className="tv-chart" role="img" aria-label={`${symbol} price history. Recent prices are also available in the table below.`}/>
    <details className="price-table"><summary>View recent prices as a table</summary>
      <p>The last 20 price observations in the selected period. Use “Download prices” for the full history.</p>
      <div className="price-table-scroll" role="region" aria-label="Recent prices" tabIndex={0}><table>
        <caption>{symbol} recent prices · source: Twelve Data</caption>
        <thead><tr>{['Date and time', 'Open', 'High', 'Low', 'Close', 'Volume'].map(label => <th scope="col" key={label}>{label}</th>)}</tr></thead>
        <tbody>{candles.slice(-20).map(candle => <tr key={candle.time}><th scope="row">{new Date(candle.time * 1000).toLocaleString()}</th><td>{currency(candle.open)}</td><td>{currency(candle.high)}</td><td>{currency(candle.low)}</td><td>{currency(candle.close)}</td><td>{Math.round(candle.volume).toLocaleString()}</td></tr>)}</tbody>
      </table></div>
    </details>
  </div>
}
