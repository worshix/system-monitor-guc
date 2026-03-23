'use client'

interface GaugeProps {
  value: number
  min: number
  max: number
  label: string
  unit: string
  color?: string
  warningAt?: number
  criticalAt?: number
}

export default function Gauge({
  value,
  min,
  max,
  label,
  unit,
  color = '#3b82f6',
  warningAt,
  criticalAt,
}: GaugeProps) {
  const clampedValue = Math.max(min, Math.min(max, value))
  const percent = (clampedValue - min) / (max - min)

  // Arc from -225deg to 45deg = 270deg sweep
  const startAngle = -225
  const sweepAngle = 270
  const angle = startAngle + sweepAngle * percent

  const cx = 100
  const cy = 100
  const r = 75

  function polarToCartesian(angleDeg: number) {
    const rad = (angleDeg * Math.PI) / 180
    return {
      x: cx + r * Math.cos(rad),
      y: cy + r * Math.sin(rad),
    }
  }

  function arcPath(startDeg: number, endDeg: number) {
    const s = polarToCartesian(startDeg)
    const e = polarToCartesian(endDeg)
    const largeArc = endDeg - startDeg > 180 ? 1 : 0
    return `M ${s.x} ${s.y} A ${r} ${r} 0 ${largeArc} 1 ${e.x} ${e.y}`
  }

  // Track arc
  const trackPath = arcPath(startAngle, startAngle + sweepAngle)

  // Value arc
  const valueEndAngle = startAngle + sweepAngle * percent
  const valuePath = percent > 0 ? arcPath(startAngle, valueEndAngle) : ''

  // Warning / critical arcs
  const warnStart =
    warningAt !== undefined
      ? startAngle + sweepAngle * ((warningAt - min) / (max - min))
      : null
  const critStart =
    criticalAt !== undefined
      ? startAngle + sweepAngle * ((criticalAt - min) / (max - min))
      : null

  // Needle tip
  const needleTip = polarToCartesian(angle)

  // Active color depending on thresholds
  let activeColor = color
  if (criticalAt !== undefined && clampedValue >= criticalAt) {
    activeColor = '#ef4444'
  } else if (warningAt !== undefined && clampedValue >= warningAt) {
    activeColor = '#f59e0b'
  }

  // Tick marks
  const ticks = Array.from({ length: 11 }, (_, i) => i / 10)

  return (
    <div className="gauge-wrapper">
      <svg viewBox="0 0 200 180" className="w-full max-w-[200px] mx-auto">
        {/* Glow filter */}
        <defs>
          <filter id={`glow-${label}`} x="-20%" y="-20%" width="140%" height="140%">
            <feGaussianBlur stdDeviation="3" result="coloredBlur" />
            <feMerge>
              <feMergeNode in="coloredBlur" />
              <feMergeNode in="SourceGraphic" />
            </feMerge>
          </filter>
          <filter id={`glow-needle-${label}`} x="-50%" y="-50%" width="200%" height="200%">
            <feGaussianBlur stdDeviation="2" result="coloredBlur" />
            <feMerge>
              <feMergeNode in="coloredBlur" />
              <feMergeNode in="SourceGraphic" />
            </feMerge>
          </filter>
        </defs>

        {/* Background arc (track) */}
        <path
          d={trackPath}
          fill="none"
          stroke="#1e3a5f"
          strokeWidth="10"
          strokeLinecap="round"
        />

        {/* Warning zone */}
        {warnStart !== null && critStart !== null && (
          <path
            d={arcPath(warnStart, critStart)}
            fill="none"
            stroke="#f59e0b22"
            strokeWidth="10"
            strokeLinecap="butt"
          />
        )}

        {/* Critical zone */}
        {critStart !== null && (
          <path
            d={arcPath(critStart, startAngle + sweepAngle)}
            fill="none"
            stroke="#ef444422"
            strokeWidth="10"
            strokeLinecap="butt"
          />
        )}

        {/* Value arc */}
        {valuePath && (
          <path
            d={valuePath}
            fill="none"
            stroke={activeColor}
            strokeWidth="10"
            strokeLinecap="round"
            filter={`url(#glow-${label})`}
            style={{ transition: 'stroke-dasharray 0.5s ease, d 0.5s ease' }}
          />
        )}

        {/* Tick marks */}
        {ticks.map((t, i) => {
          const tickAngle = startAngle + sweepAngle * t
          const inner = 60
          const outer = i % 5 === 0 ? 70 : 65
          const innerPt = {
            x: cx + inner * Math.cos((tickAngle * Math.PI) / 180),
            y: cy + inner * Math.sin((tickAngle * Math.PI) / 180),
          }
          const outerPt = {
            x: cx + outer * Math.cos((tickAngle * Math.PI) / 180),
            y: cy + outer * Math.sin((tickAngle * Math.PI) / 180),
          }
          return (
            <line
              key={i}
              x1={innerPt.x}
              y1={innerPt.y}
              x2={outerPt.x}
              y2={outerPt.y}
              stroke={i % 5 === 0 ? '#4b87c8' : '#1e3a5f'}
              strokeWidth={i % 5 === 0 ? 2 : 1}
            />
          )
        })}

        {/* Center hub */}
        <circle cx={cx} cy={cy} r={6} fill="#0f2040" stroke="#3b82f6" strokeWidth={1.5} />

        {/* Needle */}
        <line
          x1={cx}
          y1={cy}
          x2={needleTip.x}
          y2={needleTip.y}
          stroke={activeColor}
          strokeWidth={2}
          strokeLinecap="round"
          filter={`url(#glow-needle-${label})`}
          style={{ transition: 'all 0.5s ease' }}
        />

        {/* Value display */}
        <text
          x={cx}
          y={cy + 28}
          textAnchor="middle"
          fontSize="16"
          fontWeight="bold"
          fontFamily="monospace"
          fill={activeColor}
          style={{ transition: 'fill 0.3s ease' }}
        >
          {Number.isFinite(value) ? value.toFixed(1) : '—'}
        </text>

        <text
          x={cx}
          y={cy + 40}
          textAnchor="middle"
          fontSize="8"
          fontFamily="monospace"
          fill="#4b87c8"
        >
          {unit}
        </text>

        {/* Min/Max labels */}
        <text x="22" y="158" textAnchor="middle" fontSize="8" fontFamily="monospace" fill="#2d5f8a">
          {min}
        </text>
        <text x="178" y="158" textAnchor="middle" fontSize="8" fontFamily="monospace" fill="#2d5f8a">
          {max}
        </text>
      </svg>

      <p className="text-center text-xs font-mono text-blue-400 mt-1 tracking-widest uppercase">
        {label}
      </p>
    </div>
  )
}
