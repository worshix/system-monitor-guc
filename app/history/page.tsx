'use client'

import { useEffect, useState, useCallback } from 'react'
import Link from 'next/link'
import {
  AreaChart, Area, XAxis, YAxis, CartesianGrid,
  Tooltip, ResponsiveContainer, ReferenceLine,
} from 'recharts'

// ── Types ──────────────────────────────────────────────────────────
interface MotorLog {
  id:          number
  timestamp:   string
  voltage:     number
  temperature: number
  current:     number
  power:       number
  vibration:   number
  health:      number
  fault:       string
  motorState:  string
}

interface FaultEvent {
  type:     string
  start:    string
  end:      string
  durationS: number
}

// ── Helpers ────────────────────────────────────────────────────────
function fmt(d: string) {
  return new Date(d).toLocaleTimeString('en-GB', {
    hour: '2-digit', minute: '2-digit', second: '2-digit',
  })
}

function fmtDatetime(d: string) {
  return new Date(d).toLocaleString('en-GB', {
    day: '2-digit', month: 'short', hour: '2-digit',
    minute: '2-digit', second: '2-digit',
  })
}

function stats(arr: number[]) {
  if (arr.length === 0) return { min: 0, max: 0, avg: 0 }
  const min = Math.min(...arr)
  const max = Math.max(...arr)
  const avg = arr.reduce((a, b) => a + b, 0) / arr.length
  return { min, max, avg }
}

function groupFaultEvents(logs: MotorLog[]): FaultEvent[] {
  const events: FaultEvent[] = []
  let current: { type: string; start: string; end: string } | null = null

  for (const log of logs) {
    if (log.fault !== 'NONE') {
      if (!current || current.type !== log.fault) {
        if (current) {
          events.push({
            ...current,
            durationS: Math.round((new Date(current.end).getTime() - new Date(current.start).getTime()) / 1000),
          })
        }
        current = { type: log.fault, start: log.timestamp, end: log.timestamp }
      } else {
        current.end = log.timestamp
      }
    } else {
      if (current) {
        events.push({
          ...current,
          durationS: Math.round((new Date(current.end).getTime() - new Date(current.start).getTime()) / 1000),
        })
        current = null
      }
    }
  }
  if (current) {
    events.push({
      ...current,
      durationS: Math.round((new Date(current.end).getTime() - new Date(current.start).getTime()) / 1000),
    })
  }
  return events
}

const FAULT_COLORS: Record<string, string> = {
  UNDERVOLTAGE: 'text-yellow-400 border-yellow-700/50 bg-yellow-950/30',
  OVERVOLTAGE:  'text-orange-400 border-orange-700/50 bg-orange-950/30',
  HIGH_TEMP:    'text-red-400    border-red-700/50    bg-red-950/30',
}

// ── Tooltip ────────────────────────────────────────────────────────
const DarkTooltip = ({ active, payload, label }: {
  active?: boolean; payload?: { value: number; name: string }[]; label?: string
}) => {
  if (!active || !payload?.length) return null
  return (
    <div className="bg-[#0a1628] border border-blue-900/60 rounded p-2 text-xs font-mono">
      <p className="text-blue-400 mb-1">{label}</p>
      {payload.map((p, i) => (
        <p key={i} className="text-slate-300">{p.value?.toFixed(3)}</p>
      ))}
    </div>
  )
}

// ── Chart Card ─────────────────────────────────────────────────────
function ChartCard({
  title, unit, dataKey, data, color, refLines, domain,
}: {
  title:     string
  unit:      string
  dataKey:   string
  data:      Record<string, unknown>[]
  color:     string
  refLines?: { y: number; label: string; color: string }[]
  domain?:   [number | 'auto', number | 'auto']
}) {
  return (
    <div className="p-4 rounded-lg border border-blue-900/40 bg-[#0a1628]/80">
      <div className="flex items-center justify-between mb-3">
        <h3 className="text-[10px] text-blue-500 tracking-widest uppercase">{title}</h3>
        <span className="text-[10px] text-slate-600 font-mono">{unit}</span>
      </div>
      {data.length === 0 ? (
        <div className="h-40 flex items-center justify-center text-slate-600 text-xs tracking-widest uppercase">
          No data
        </div>
      ) : (
        <ResponsiveContainer width="100%" height={160}>
          <AreaChart data={data} margin={{ top: 4, right: 4, bottom: 0, left: -20 }}>
            <defs>
              <linearGradient id={`grad-${dataKey}`} x1="0" y1="0" x2="0" y2="1">
                <stop offset="10%"  stopColor={color} stopOpacity={0.25} />
                <stop offset="95%"  stopColor={color} stopOpacity={0} />
              </linearGradient>
            </defs>
            <CartesianGrid strokeDasharray="3 3" stroke="#1e3a5f" />
            <XAxis
              dataKey="time"
              tick={{ fill: '#4b87c8', fontSize: 9, fontFamily: 'monospace' }}
              interval="preserveStartEnd"
              tickLine={false}
            />
            <YAxis
              domain={domain ?? ['auto', 'auto']}
              tick={{ fill: '#4b87c8', fontSize: 9, fontFamily: 'monospace' }}
              tickLine={false}
              axisLine={false}
              width={36}
            />
            <Tooltip content={<DarkTooltip />} />
            {refLines?.map((r) => (
              <ReferenceLine
                key={r.y}
                y={r.y}
                stroke={r.color}
                strokeDasharray="4 4"
                strokeOpacity={0.6}
                label={{ value: r.label, fill: r.color, fontSize: 8, fontFamily: 'monospace' }}
              />
            ))}
            <Area
              type="monotone"
              dataKey={dataKey}
              stroke={color}
              strokeWidth={1.5}
              fill={`url(#grad-${dataKey})`}
              dot={false}
              isAnimationActive={false}
            />
          </AreaChart>
        </ResponsiveContainer>
      )}
    </div>
  )
}

// ── Stat Card ──────────────────────────────────────────────────────
function StatCard({
  label, unit, min, max, avg, decimals = 2,
}: {
  label: string; unit: string; min: number; max: number; avg: number; decimals?: number
}) {
  return (
    <div className="p-3 rounded-lg border border-blue-900/40 bg-[#0a1628]/80">
      <p className="text-[10px] text-blue-500 tracking-widest uppercase mb-2">{label}</p>
      <div className="grid grid-cols-3 gap-1 text-xs font-mono">
        <div>
          <p className="text-slate-600 text-[9px] uppercase">Min</p>
          <p className="text-blue-300 font-bold">{min.toFixed(decimals)}</p>
          <p className="text-slate-600 text-[9px]">{unit}</p>
        </div>
        <div>
          <p className="text-slate-600 text-[9px] uppercase">Avg</p>
          <p className="text-slate-300 font-bold">{avg.toFixed(decimals)}</p>
          <p className="text-slate-600 text-[9px]">{unit}</p>
        </div>
        <div>
          <p className="text-slate-600 text-[9px] uppercase">Max</p>
          <p className="text-blue-300 font-bold">{max.toFixed(decimals)}</p>
          <p className="text-slate-600 text-[9px]">{unit}</p>
        </div>
      </div>
    </div>
  )
}

// ── Page ───────────────────────────────────────────────────────────
const HOURS_OPTIONS = [
  { label: '1 h',  value: 1  },
  { label: '6 h',  value: 6  },
  { label: '24 h', value: 24 },
  { label: 'All',  value: 720 },
]

export default function HistoryPage() {
  const [logs, setLogs]     = useState<MotorLog[]>([])
  const [hours, setHours]   = useState(1)
  const [loading, setLoading] = useState(false)
  const [lastFetch, setLastFetch] = useState<Date | null>(null)

  const fetchLogs = useCallback(async () => {
    setLoading(true)
    try {
      const res = await fetch(`/api/logs?hours=${hours}&limit=1000`)
      if (res.ok) {
        const data: MotorLog[] = await res.json()
        setLogs(data)
        setLastFetch(new Date())
      }
    } catch (e) {
      console.error('Failed to fetch logs:', e)
    } finally {
      setLoading(false)
    }
  }, [hours])

  useEffect(() => { fetchLogs() }, [fetchLogs])

  // ── Chart-ready data ──────────────────────────────────────────
  const chartData = logs.map((l) => ({
    time:        fmt(l.timestamp),
    voltage:     l.voltage,
    temperature: l.temperature,
    current:     l.current,
    power:       l.power,
    vibration:   l.vibration,
    health:      l.health,
  }))

  // ── Summary stats ─────────────────────────────────────────────
  const voltageStats  = stats(logs.map((l) => l.voltage))
  const tempStats     = stats(logs.map((l) => l.temperature))
  const currentStats  = stats(logs.map((l) => l.current))
  const powerStats    = stats(logs.map((l) => l.power))
  const vibStats      = stats(logs.map((l) => l.vibration))

  const faultCount = logs.filter((l) => l.fault !== 'NONE').length
  const motorOnPct = logs.length > 0
    ? Math.round((logs.filter((l) => l.motorState === 'ON').length / logs.length) * 100)
    : 0

  // ── Fault events ──────────────────────────────────────────────
  const faultEvents = groupFaultEvents(logs)

  return (
    <div className="min-h-screen bg-[#060d1a] text-slate-200 font-mono p-4 md:p-8">

      {/* ── Header ─────────────────────────────────────────────── */}
      <header className="mb-6 border-b border-blue-900/60 pb-4">
        <div className="flex flex-col sm:flex-row sm:items-center sm:justify-between gap-3">
          <div>
            <div className="flex items-center gap-3 mb-1">
              <Link
                href="/"
                className="text-[10px] text-blue-500 hover:text-blue-300 tracking-widest uppercase border border-blue-900/40 hover:border-blue-700/60 rounded px-2 py-1 transition-colors"
              >
                ← Dashboard
              </Link>
            </div>
            <h1 className="text-2xl font-bold tracking-[0.2em] text-blue-300 uppercase">
              Motor History
            </h1>
            <p className="text-xs text-blue-600 tracking-widest mt-0.5">
              {logs.length} records — {lastFetch ? `refreshed ${lastFetch.toLocaleTimeString()}` : 'loading…'}
            </p>
          </div>

          {/* Time range + refresh */}
          <div className="flex items-center gap-2 flex-wrap">
            {HOURS_OPTIONS.map((o) => (
              <button
                key={o.value}
                onClick={() => setHours(o.value)}
                className={`px-3 py-1.5 rounded text-xs tracking-widest uppercase border transition-colors ${
                  hours === o.value
                    ? 'bg-blue-700 border-blue-500/60 text-white'
                    : 'bg-transparent border-blue-900/40 text-blue-400 hover:border-blue-700/60'
                }`}
              >
                {o.label}
              </button>
            ))}
            <button
              onClick={fetchLogs}
              disabled={loading}
              className="px-3 py-1.5 rounded text-xs tracking-widest uppercase border border-blue-900/40 text-blue-400 hover:border-blue-700/60 disabled:opacity-40 transition-colors"
            >
              {loading ? '…' : '↺ Refresh'}
            </button>
          </div>
        </div>
      </header>

      {/* ── Quick Stats Bar ──────────────────────────────────────── */}
      <section className="mb-6 grid grid-cols-2 sm:grid-cols-4 gap-3">
        <QuickStat label="Total Readings"  value={String(logs.length)} />
        <QuickStat label="Fault Readings"  value={String(faultCount)}  danger={faultCount > 0} />
        <QuickStat label="Fault Events"    value={String(faultEvents.length)} danger={faultEvents.length > 0} />
        <QuickStat label="Motor On"        value={`${motorOnPct} %`}   highlight={motorOnPct > 0} />
      </section>

      {/* ── Charts ───────────────────────────────────────────────── */}
      <section className="mb-6 grid grid-cols-1 md:grid-cols-2 xl:grid-cols-3 gap-4">
        <ChartCard
          title="Voltage" unit="V" dataKey="voltage" data={chartData}
          color="#38bdf8" domain={[0, 280]}
          refLines={[
            { y: 210, label: '210 V', color: '#f59e0b' },
            { y: 250, label: '250 V', color: '#f59e0b' },
          ]}
        />
        <ChartCard
          title="Temperature" unit="°C" dataKey="temperature" data={chartData}
          color="#f97316"
          refLines={[{ y: 50, label: '50 °C', color: '#ef4444' }]}
        />
        <ChartCard
          title="Current" unit="A" dataKey="current" data={chartData}
          color="#60a5fa" domain={[0, 0.5]}
        />
        <ChartCard
          title="Apparent Power" unit="VA" dataKey="power" data={chartData}
          color="#818cf8" domain={[0, 50]}
        />
        <ChartCard
          title="Vibration" unit="mm/s" dataKey="vibration" data={chartData}
          color="#3b82f6" domain={[0, 5]}
          refLines={[{ y: 2.8, label: '2.8 mm/s', color: '#f59e0b' }]}
        />
        <ChartCard
          title="Health" unit="%" dataKey="health" data={chartData}
          color="#34d399" domain={[0, 100]}
          refLines={[{ y: 50, label: '50 %', color: '#f59e0b' }]}
        />
      </section>

      {/* ── Extreme Values Summary ───────────────────────────────── */}
      <section className="mb-6">
        <h2 className="text-[10px] text-blue-500 tracking-widest uppercase mb-3">Extreme Values</h2>
        <div className="grid grid-cols-2 sm:grid-cols-3 lg:grid-cols-5 gap-3">
          <StatCard label="Voltage"     unit="V"    {...voltageStats} decimals={1} />
          <StatCard label="Temperature" unit="°C"   {...tempStats}    decimals={1} />
          <StatCard label="Current"     unit="A"    {...currentStats} decimals={3} />
          <StatCard label="Power"       unit="VA"   {...powerStats}   decimals={1} />
          <StatCard label="Vibration"   unit="mm/s" {...vibStats}     decimals={2} />
        </div>
      </section>

      {/* ── Fault Event Log ─────────────────────────────────────── */}
      <section>
        <h2 className="text-[10px] text-blue-500 tracking-widest uppercase mb-3">
          Fault Events {faultEvents.length > 0 && (
            <span className="text-red-400 ml-2">({faultEvents.length})</span>
          )}
        </h2>

        {faultEvents.length === 0 ? (
          <div className="p-6 rounded-lg border border-blue-900/40 bg-[#0a1628]/80 text-center">
            <p className="text-emerald-400 text-xs tracking-widest uppercase">✓ No fault events in this period</p>
          </div>
        ) : (
          <div className="rounded-lg border border-blue-900/40 bg-[#0a1628]/80 overflow-hidden">
            <table className="w-full text-xs font-mono">
              <thead>
                <tr className="border-b border-blue-900/40">
                  <th className="text-left p-3 text-[10px] text-blue-500 tracking-widest uppercase">Fault</th>
                  <th className="text-left p-3 text-[10px] text-blue-500 tracking-widest uppercase">Started</th>
                  <th className="text-left p-3 text-[10px] text-blue-500 tracking-widest uppercase">Last Seen</th>
                  <th className="text-right p-3 text-[10px] text-blue-500 tracking-widest uppercase">Duration</th>
                </tr>
              </thead>
              <tbody>
                {[...faultEvents].reverse().map((ev, i) => (
                  <tr
                    key={i}
                    className={`border-b border-blue-900/20 last:border-0 ${
                      FAULT_COLORS[ev.type] ?? 'text-slate-400 border-slate-700/30 bg-transparent'
                    }`}
                  >
                    <td className="p-3 font-bold tracking-widest uppercase">{ev.type}</td>
                    <td className="p-3 text-slate-400">{fmtDatetime(ev.start)}</td>
                    <td className="p-3 text-slate-400">{fmtDatetime(ev.end)}</td>
                    <td className="p-3 text-right tabular-nums">
                      {ev.durationS < 60
                        ? `${ev.durationS} s`
                        : `${Math.floor(ev.durationS / 60)} m ${ev.durationS % 60} s`}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>
    </div>
  )
}

function QuickStat({
  label, value, highlight, danger,
}: {
  label: string; value: string; highlight?: boolean; danger?: boolean
}) {
  return (
    <div className="p-3 rounded-lg border border-blue-900/40 bg-[#0a1628]/80">
      <p className="text-[10px] text-blue-600 tracking-widest uppercase mb-1">{label}</p>
      <p className={`text-xl font-bold ${danger ? 'text-red-400' : highlight ? 'text-blue-300' : 'text-slate-300'}`}>
        {value}
      </p>
    </div>
  )
}
