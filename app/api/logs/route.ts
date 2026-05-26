import { NextRequest, NextResponse } from "next/server";
import { prisma } from "@/lib/prisma";

// POST /api/logs — save a new sensor snapshot
export async function POST(req: NextRequest) {
  try {
    const body = await req.json();
    const {
      voltage = 0,
      temperature = 0,
      current = 0,
      power = 0,
      vibration = 0,
      health = 100,
      fault = "NONE",
      motorState = "OFF",
    } = body;

    const log = await prisma.motorLog.create({
      data: { voltage, temperature, current, power, vibration, health, fault, motorState },
    });

    return NextResponse.json(log, { status: 201 });
  } catch (err) {
    console.error("[POST /api/logs]", err);
    return NextResponse.json({ error: "Failed to save log" }, { status: 500 });
  }
}

// GET /api/logs — fetch historical logs
// Query params:
//   hours=N   → last N hours (default 1)
//   limit=N   → max records (default 500, max 2000)
export async function GET(req: NextRequest) {
  try {
    const { searchParams } = new URL(req.url);
    const hours = Math.max(1, parseInt(searchParams.get("hours") ?? "1", 10));
    const limit = Math.min(2000, parseInt(searchParams.get("limit") ?? "500", 10));

    const since = new Date(Date.now() - hours * 60 * 60 * 1000);

    const logs = await prisma.motorLog.findMany({
      where: { timestamp: { gte: since } },
      orderBy: { timestamp: "asc" },
      take: limit,
    });

    return NextResponse.json(logs);
  } catch (err) {
    console.error("[GET /api/logs]", err);
    return NextResponse.json({ error: "Failed to fetch logs" }, { status: 500 });
  }
}

// DELETE /api/logs — clear all logs older than N hours (default: all)
export async function DELETE(req: NextRequest) {
  try {
    const { searchParams } = new URL(req.url);
    const olderThanHours = searchParams.get("olderThan");
    const where = olderThanHours
      ? { timestamp: { lt: new Date(Date.now() - parseInt(olderThanHours, 10) * 3600_000) } }
      : {};

    const { count } = await prisma.motorLog.deleteMany({ where });
    return NextResponse.json({ deleted: count });
  } catch (err) {
    console.error("[DELETE /api/logs]", err);
    return NextResponse.json({ error: "Failed to delete logs" }, { status: 500 });
  }
}
