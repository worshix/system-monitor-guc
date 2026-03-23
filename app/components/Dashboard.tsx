'use client'

import { useEffect, useRef, useState, useCallback } from 'react'
import mqtt from 'mqtt'
import Gauge from './Gauge'

const HEARTBEAT_TIMEOUT_MS = 10_000

const TOPICS = [
  '/motor/vibration',
  '/motor/temperature',
  '/motor/current',
  '/motor/power',
  '/motor/health',
] as const

interface SystemInfo {
  title: string
  voltage: number
}

const SYSTEM_INFO: SystemInfo = {
  title: 'GUC Motor Monitor',
  voltage: 220,
}

type BrokerStatus = 'disconnected' | 'connecting' | 'connected' | 'error'

interface MotorData {
  vibration: number
  temperature: number
  current: number
  power: number
  health: number
}

const DEFAULT_DATA: MotorData = {
  vibration: 0,
  temperature: 0,
  current: 0,
  power: 0,
  health: 0,
}

function StatusDot({ status }: { status: BrokerStatus }) {
  const colors: Record<BrokerStatus, string> = {
    connected: 'bg-emerald-400 shadow-emerald-400',
    connecting: 'bg-yellow-400 shadow-yellow-400',
    disconnected: 'bg-slate-500 shadow-slate-500',
    error: 'bg-red-500 shadow-red-500',
  }
  const labels: Record<BrokerStatus, string> = {
    connected: 'Broker Online',
    connecting: 'Connecting…',
    disconnected: 'Broker Offline',
    error: 'Broker Error',
  }
  return (
    <div className="flex items-center gap-2">
      <span
        className={`w-2.5 h-2.5 rounded-full ${colors[status]} shadow-[0_0_6px_2px] ${status === 'connected' ? 'animate-pulse' : ''}`}
      />
      <span className="text-xs font-mono tracking-widest text-blue-300 uppercase">
        {labels[status]}
      </span>
    </div>
  )
}

function HeartbeatDot({ connected }: { connected: boolean }) {
  return (
    <div className="flex items-center gap-2">
      <span
        className={`w-2.5 h-2.5 rounded-full ${connected ? 'bg-blue-400 shadow-blue-400 shadow-[0_0_6px_2px] animate-pulse' : 'bg-slate-600'}`}
      />
      <span className="text-xs font-mono tracking-widest text-blue-300 uppercase">
        {connected ? 'Motor Online' : 'Motor Offline'}
      </span>
    </div>
  )
}

export default function Dashboard() {
  const [brokerUrl, setBrokerUrl] = useState('ws://localhost:9001')
  const [brokerStatus, setBrokerStatus] = useState<BrokerStatus>('disconnected')
  const [motorConnected, setMotorConnected] = useState(false)
  const [data, setData] = useState<MotorData>(DEFAULT_DATA)
  const [motorRunning, setMotorRunning] = useState(false)
  const [lastSeen, setLastSeen] = useState<Date | null>(null)

  const clientRef = useRef<ReturnType<typeof mqtt.connect> | null>(null)
  const heartbeatTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)

  const resetHeartbeat = useCallback(() => {
    setMotorConnected(true)
    setLastSeen(new Date())
    if (heartbeatTimerRef.current) clearTimeout(heartbeatTimerRef.current)
    heartbeatTimerRef.current = setTimeout(() => {
      setMotorConnected(false)
    }, HEARTBEAT_TIMEOUT_MS)
  }, [])

  const connect = useCallback(() => {
    if (clientRef.current) {
      clientRef.current.end(true)
      clientRef.current = null
    }
    setBrokerStatus('connecting')

    const client = mqtt.connect(brokerUrl, {
      reconnectPeriod: 5000,
      connectTimeout: 10000,
    })
    clientRef.current = client

    client.on('connect', () => {
      setBrokerStatus('connected')
      TOPICS.forEach((t) => client.subscribe(t))
    })

    client.on('reconnect', () => setBrokerStatus('connecting'))
    client.on('error', () => setBrokerStatus('error'))
    client.on('close', () => setBrokerStatus('disconnected'))
    client.on('offline', () => setBrokerStatus('disconnected'))

    client.on('message', (topic: string, payload: Buffer) => {
      const raw = parseFloat(payload.toString())
      if (!Number.isFinite(raw)) return

      resetHeartbeat()

      setData((prev) => {
        const next = { ...prev }
        switch (topic) {
          case '/motor/vibration':
            next.vibration = raw
            break
          case '/motor/temperature':
            next.temperature = raw
            break
          case '/motor/current':
            next.current = raw
            next.power = SYSTEM_INFO.voltage * raw
            break
          case '/motor/power':
            next.power = raw
            break
          case '/motor/health':
            next.health = raw
            break
        }
        return next
      })
    })
  }, [brokerUrl, resetHeartbeat])

  const disconnect = useCallback(() => {
    clientRef.current?.end(true)
    clientRef.current = null
    setBrokerStatus('disconnected')
    setMotorConnected(false)
    if (heartbeatTimerRef.current) clearTimeout(heartbeatTimerRef.current)
  }, [])

  const sendCommand = useCallback((cmd: 'start' | 'stop') => {
    clientRef.current?.publish('/motor/command', cmd)
    setMotorRunning(cmd === 'start')
  }, [])

  useEffect(() => {
    return () => {
      clientRef.current?.end(true)
      if (heartbeatTimerRef.current) clearTimeout(heartbeatTimerRef.current)
    }
  }, [])

  const calculatedPower = SYSTEM_INFO.voltage * data.current

  return (
    <div className="min-h-screen bg-[#060d1a] text-slate-200 font-mono p-4 md:p-8">
      {/* ── Header ── */}
      <header className="mb-6 border-b border-blue-900/60 pb-4">
        <div className="flex flex-col sm:flex-row sm:items-center sm:justify-between gap-4">
          <div>
            <h1 className="text-2xl font-bold tracking-[0.2em] text-blue-300 uppercase">
              {SYSTEM_INFO.title}
            </h1>
            <p className="text-xs text-blue-600 tracking-widest mt-0.5">
              Industrial Motor Monitoring System
            </p>
          </div>
          <div className="flex flex-col gap-1.5 items-start sm:items-end">
            <StatusDot status={brokerStatus} />
            <HeartbeatDot connected={motorConnected} />
            {lastSeen && (
              <span className="text-[10px] text-slate-600 tracking-wide">
                Last seen: {lastSeen.toLocaleTimeString()}
              </span>
            )}
          </div>
        </div>
      </header>

      {/* ── Connection Panel ── */}
      <section className="mb-6 p-4 rounded-lg border border-blue-900/40 bg-[#0a1628]/80 flex flex-col sm:flex-row gap-3 items-stretch sm:items-end">
        <div className="flex-1">
          <label className="block text-[10px] text-blue-500 tracking-widest mb-1 uppercase">
            Broker WebSocket URL
          </label>
          <input
            type="text"
            value={brokerUrl}
            onChange={(e) => setBrokerUrl(e.target.value)}
            className="w-full bg-[#060d1a] border border-blue-900/60 rounded px-3 py-2 text-sm text-blue-200 outline-none focus:border-blue-500 transition-colors"
            placeholder="ws://localhost:9001"
          />
        </div>
        <div className="flex gap-2">
          <button
            onClick={connect}
            disabled={brokerStatus === 'connected' || brokerStatus === 'connecting'}
            className="px-4 py-2 rounded bg-blue-700 hover:bg-blue-600 disabled:opacity-40 disabled:cursor-not-allowed text-sm tracking-widest transition-all duration-200 uppercase border border-blue-500/30"
          >
            Connect
          </button>
          <button
            onClick={disconnect}
            disabled={brokerStatus === 'disconnected'}
            className="px-4 py-2 rounded bg-slate-800 hover:bg-slate-700 disabled:opacity-40 disabled:cursor-not-allowed text-sm tracking-widest transition-all duration-200 uppercase border border-slate-600/30"
          >
            Disconnect
          </button>
        </div>
      </section>

      {/* ── System Info ── */}
      <section className="mb-6 p-4 rounded-lg border border-blue-900/40 bg-[#0a1628]/80">
        <h2 className="text-[10px] text-blue-500 tracking-widest uppercase mb-3">System Info</h2>
        <div className="grid grid-cols-2 sm:grid-cols-4 gap-4 text-sm">
          <InfoField label="System Title" value={SYSTEM_INFO.title} />
          <InfoField label="Supply Voltage" value={`${SYSTEM_INFO.voltage} V`} />
          <InfoField
            label="Motor State"
            value={motorRunning ? 'RUNNING' : 'STOPPED'}
            highlight={motorRunning}
          />
          <InfoField
            label="Health Score"
            value={`${data.health.toFixed(1)} %`}
            highlight={data.health > 70}
          />
        </div>
      </section>

      {/* ── Gauges ── */}
      <section className="mb-6 grid grid-cols-2 sm:grid-cols-3 lg:grid-cols-5 gap-4">
        <GaugeCard>
          <Gauge
            value={data.vibration}
            min={0}
            max={10}
            label="Vibration"
            unit="mm/s"
            color="#3b82f6"
            warningAt={6}
            criticalAt={8}
          />
        </GaugeCard>
        <GaugeCard>
          <Gauge
            value={data.temperature}
            min={0}
            max={150}
            label="Temperature"
            unit="°C"
            color="#38bdf8"
            warningAt={80}
            criticalAt={110}
          />
        </GaugeCard>
        <GaugeCard>
          <Gauge
            value={data.current}
            min={0}
            max={50}
            label="Current"
            unit="A"
            color="#60a5fa"
            warningAt={35}
            criticalAt={45}
          />
        </GaugeCard>
        <GaugeCard>
          <Gauge
            value={calculatedPower}
            min={0}
            max={SYSTEM_INFO.voltage * 50}
            label="Power"
            unit="W"
            color="#818cf8"
            warningAt={SYSTEM_INFO.voltage * 35}
            criticalAt={SYSTEM_INFO.voltage * 45}
          />
        </GaugeCard>
        <GaugeCard>
          <Gauge
            value={data.health}
            min={0}
            max={100}
            label="Health"
            unit="%"
            color="#34d399"
            warningAt={40}
            criticalAt={20}
          />
        </GaugeCard>
      </section>

      {/* ── Motor Control ── */}
      <section className="flex justify-center">
        <div className="p-6 rounded-xl border border-blue-900/40 bg-[#0a1628]/80 flex flex-col items-center gap-4 min-w-60">
          <h2 className="text-[10px] text-blue-500 tracking-widest uppercase">Motor Control</h2>

          <div className="relative flex items-center justify-center">
            <div
              className={`w-20 h-20 rounded-full border-4 flex items-center justify-center transition-all duration-700 ${
                motorRunning
                  ? 'border-blue-500 shadow-[0_0_24px_4px_rgba(59,130,246,0.4)]'
                  : 'border-slate-700'
              }`}
            >
              <svg viewBox="0 0 40 40" className="w-10 h-10">
                <circle
                  cx="20"
                  cy="20"
                  r="14"
                  fill="none"
                  stroke={motorRunning ? '#3b82f6' : '#1e3a5f'}
                  strokeWidth="2"
                />
                {motorRunning ? (
                  <g
                    style={{
                      transformOrigin: '20px 20px',
                      animation: 'spin 2s linear infinite',
                    }}
                  >
                    <line x1="20" y1="8" x2="20" y2="20" stroke="#3b82f6" strokeWidth="2.5" strokeLinecap="round" />
                    <line x1="20" y1="20" x2="30" y2="28" stroke="#3b82f6" strokeWidth="2.5" strokeLinecap="round" />
                    <line x1="20" y1="20" x2="10" y2="28" stroke="#3b82f6" strokeWidth="2.5" strokeLinecap="round" />
                  </g>
                ) : (
                  <rect x="15" y="15" width="10" height="10" rx="1" fill="#1e3a5f" />
                )}
              </svg>
            </div>
          </div>

          <p className="text-sm tracking-widest text-blue-300 uppercase">
            {motorRunning ? 'Running' : 'Stopped'}
          </p>

          <div className="flex gap-3">
            <button
              onClick={() => sendCommand('start')}
              disabled={motorRunning || brokerStatus !== 'connected'}
              className="px-5 py-2.5 rounded-lg bg-blue-700 hover:bg-blue-500 disabled:opacity-30 disabled:cursor-not-allowed text-sm tracking-widest uppercase transition-all duration-200 border border-blue-500/40 shadow-[0_0_10px_rgba(59,130,246,0.2)] hover:shadow-[0_0_16px_rgba(59,130,246,0.5)]"
            >
              Start
            </button>
            <button
              onClick={() => sendCommand('stop')}
              disabled={!motorRunning || brokerStatus !== 'connected'}
              className="px-5 py-2.5 rounded-lg bg-slate-800 hover:bg-red-900/60 disabled:opacity-30 disabled:cursor-not-allowed text-sm tracking-widest uppercase transition-all duration-200 border border-slate-600/40"
            >
              Stop
            </button>
          </div>
        </div>
      </section>

      <style>{`
        @keyframes spin {
          from { transform: rotate(0deg); }
          to { transform: rotate(360deg); }
        }
      `}</style>
    </div>
  )
}

function GaugeCard({ children }: { children: React.ReactNode }) {
  return (
    <div className="p-3 rounded-lg border border-blue-900/40 bg-[#0a1628]/80 hover:border-blue-700/60 transition-colors duration-300">
      {children}
    </div>
  )
}

function InfoField({
  label,
  value,
  highlight,
}: {
  label: string
  value: string
  highlight?: boolean
}) {
  return (
    <div>
      <p className="text-[10px] text-blue-600 tracking-widest uppercase mb-0.5">{label}</p>
      <p className={`text-sm font-bold tracking-wide ${highlight ? 'text-blue-300' : 'text-slate-300'}`}>
        {value}
      </p>
    </div>
  )
}
