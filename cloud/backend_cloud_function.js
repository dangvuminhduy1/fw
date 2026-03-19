/*
 * VGU Thesis
 * Project: ESP32-S3 Cloud Function 
*/
const { onRequest } = require("firebase-functions/v2/https");
const { onValueCreated } = require("firebase-functions/v2/database");
const logger = require("firebase-functions/logger");
const admin = require("firebase-admin");

// Default app (Firestore default project)
admin.initializeApp();

// Secondary app for Thesis RTDB instance
const thesisApp = admin.initializeApp(
  {
    databaseURL: "https://esp32-iot-demo-thesis-01.asia-southeast1.firebasedatabase.app",
  },
  "thesis-rtdb"
);
const rtdbThesis = thesisApp.database();

const db = admin.firestore();
const { FieldValue, Timestamp } = admin.firestore;

const EXPECTED_DEVICE_KEY = "abc123";

/**
 * =========================
 * Thresholds 
 * =========================
 */
const THRESH = {
  salinity: { ok: { min: 1.5, max: 3.0 }, warn: { min: 1.0, max: 3.5 } }, // ppt
  temperature: { ok: { min: 20.0, max: 30.0 }, warn: { min: 18.0, max: 32.0 } }, // °C
  ph: { ok: { min: 6.0, max: 7.5 }, warn: { min: 5.5, max: 8.0 } },
  batteryPct: { okMin: 40.0, warnMin: 25.0 },
};

function evaluateStatus({ sal, temp, ph, batteryPct }) {
  const alerts = [];
  let hasWarn = false;
  let hasAlert = false;

  const evalRange = (v, ok, warn, lowAlertCode, highAlertCode, lowWarnCode, highWarnCode) => {
    if (!Number.isFinite(v)) return;

    // ALERT: outside WARN range
    if (v < warn.min) { alerts.push(lowAlertCode); hasAlert = true; return; }
    if (v > warn.max) { alerts.push(highAlertCode); hasAlert = true; return; }

    // WARN: outside OK range but inside WARN range
    if (v < ok.min) { alerts.push(lowWarnCode); hasWarn = true; return; }
    if (v > ok.max) { alerts.push(highWarnCode); hasWarn = true; return; }
  };

  evalRange(sal, THRESH.salinity.ok, THRESH.salinity.warn,
    "SAL_LOW_ALERT", "SAL_HIGH_ALERT", "SAL_LOW_WARN", "SAL_HIGH_WARN");

  evalRange(temp, THRESH.temperature.ok, THRESH.temperature.warn,
    "TEMP_LOW_ALERT", "TEMP_HIGH_ALERT", "TEMP_LOW_WARN", "TEMP_HIGH_WARN");

  evalRange(ph, THRESH.ph.ok, THRESH.ph.warn,
    "PH_LOW_ALERT", "PH_HIGH_ALERT", "PH_LOW_WARN", "PH_HIGH_WARN");

  if (Number.isFinite(batteryPct)) {
    if (batteryPct < THRESH.batteryPct.warnMin) {
      alerts.push("BATTERY_LOW_ALERT");
      hasAlert = true;
    } else if (batteryPct < THRESH.batteryPct.okMin) {
      alerts.push("BATTERY_LOW_WARN");
      hasWarn = true;
    }
  }

  const status = hasAlert ? "ALERT" : (hasWarn ? "WARN" : "OK");
  return { status, alerts };
}

/**
 * =========================
 * Stats aggregation (5m / hourly / daily) - timezone UTC+7 + bucketStartSec/bucketStart
 * =========================
 */
function pad2(n) { return String(n).padStart(2, "0"); }

// Shift +7h then use UTC getters => stable regardless server timezone
function toLocalDateUTC7(sec) {
  return new Date((sec + 7 * 3600) * 1000);
}

function dailyBucket(sec) {
  const d = toLocalDateUTC7(sec);
  const y = d.getUTCFullYear();
  const m = d.getUTCMonth() + 1;
  const day = d.getUTCDate();
  const bucketStartSec = Date.UTC(y, m - 1, day, 0, 0, 0) / 1000 - 7 * 3600;
  const key = `${y}${pad2(m)}${pad2(day)}`; // YYYYMMDD
  return { key, bucketStartSec };
}

function hourlyBucket(sec) {
  const d = toLocalDateUTC7(sec);
  const y = d.getUTCFullYear();
  const m = d.getUTCMonth() + 1;
  const day = d.getUTCDate();
  const h = d.getUTCHours();
  const bucketStartSec = Date.UTC(y, m - 1, day, h, 0, 0) / 1000 - 7 * 3600;
  const key = `${y}${pad2(m)}${pad2(day)}${pad2(h)}`; // YYYYMMDDHH
  return { key, bucketStartSec };
}

function min5Bucket(sec) {
  const d = toLocalDateUTC7(sec);
  const y = d.getUTCFullYear();
  const m = d.getUTCMonth() + 1;
  const day = d.getUTCDate();
  const h = d.getUTCHours();
  const mm = d.getUTCMinutes();
  const m5 = Math.floor(mm / 5) * 5;
  const bucketStartSec = Date.UTC(y, m - 1, day, h, m5, 0) / 1000 - 7 * 3600;
  const key = `${y}${pad2(m)}${pad2(day)}${pad2(h)}${pad2(m5)}`; // YYYYMMDDHHmm
  return { key, bucketStartSec };
}

function addAgg(map, key, bucketStartSec, sal, temp, ph, battPct, battVolt) {
  const cur = map.get(key) || {
    bucketStartSec,
    count: 0,
    salinity:    { sum: 0, min: Infinity, max: -Infinity },
    temperature: { sum: 0, min: Infinity, max: -Infinity },
    ph:          { sum: 0, min: Infinity, max: -Infinity },
    batteryPct:  { sum: 0, min: Infinity, max: -Infinity },
    batteryVolt: { sum: 0, min: Infinity, max: -Infinity },
  };

  cur.count += 1;

  cur.salinity.sum += sal;
  cur.salinity.min = Math.min(cur.salinity.min, sal);
  cur.salinity.max = Math.max(cur.salinity.max, sal);

  cur.temperature.sum += temp;
  cur.temperature.min = Math.min(cur.temperature.min, temp);
  cur.temperature.max = Math.max(cur.temperature.max, temp);

  cur.ph.sum += ph;
  cur.ph.min = Math.min(cur.ph.min, ph);
  cur.ph.max = Math.max(cur.ph.max, ph);

  cur.batteryPct.sum += battPct;
  cur.batteryPct.min = Math.min(cur.batteryPct.min, battPct);
  cur.batteryPct.max = Math.max(cur.batteryPct.max, battPct);

  cur.batteryVolt.sum += battVolt;
  cur.batteryVolt.min = Math.min(cur.batteryVolt.min, battVolt);
  cur.batteryVolt.max = Math.max(cur.batteryVolt.max, battVolt);

  map.set(key, cur);
}

async function upsertAgg(docRef, agg) {
  await db.runTransaction(async (tx) => {
    const snap = await tx.get(docRef);
    const prev = snap.exists ? snap.data() : null;

    const prevCount = Number(prev?.count) || 0;

    const prevSumSalinity     = Number(prev?.sumSalinity) || 0;
    const prevSumTemperature  = Number(prev?.sumTemperature) || 0;
    const prevSumPh           = Number(prev?.sumPh) || 0;
    const prevSumBatteryPct   = Number(prev?.sumBatteryPct) || 0;
    const prevSumBatteryVolt  = Number(prev?.sumBatteryVolt) || 0;

    const next = {
      count: prevCount + agg.count,

      sumSalinity:     prevSumSalinity     + agg.salinity.sum,
      sumTemperature:  prevSumTemperature  + agg.temperature.sum,
      sumPh:           prevSumPh           + agg.ph.sum,
      sumBatteryPct:   prevSumBatteryPct   + agg.batteryPct.sum,
      sumBatteryVolt:  prevSumBatteryVolt  + agg.batteryVolt.sum,

      minSalinity: Number.isFinite(prev?.minSalinity)
        ? Math.min(prev.minSalinity, agg.salinity.min)
        : agg.salinity.min,
      maxSalinity: Number.isFinite(prev?.maxSalinity)
        ? Math.max(prev.maxSalinity, agg.salinity.max)
        : agg.salinity.max,

      minTemperature: Number.isFinite(prev?.minTemperature)
        ? Math.min(prev.minTemperature, agg.temperature.min)
        : agg.temperature.min,
      maxTemperature: Number.isFinite(prev?.maxTemperature)
        ? Math.max(prev.maxTemperature, agg.temperature.max)
        : agg.temperature.max,

      minPh: Number.isFinite(prev?.minPh)
        ? Math.min(prev.minPh, agg.ph.min)
        : agg.ph.min,
      maxPh: Number.isFinite(prev?.maxPh)
        ? Math.max(prev.maxPh, agg.ph.max)
        : agg.ph.max,

      minBatteryPct: Number.isFinite(prev?.minBatteryPct)
        ? Math.min(prev.minBatteryPct, agg.batteryPct.min)
        : agg.batteryPct.min,
      maxBatteryPct: Number.isFinite(prev?.maxBatteryPct)
        ? Math.max(prev.maxBatteryPct, agg.batteryPct.max)
        : agg.batteryPct.max,

      minBatteryVolt: Number.isFinite(prev?.minBatteryVolt)
        ? Math.min(prev.minBatteryVolt, agg.batteryVolt.min)
        : agg.batteryVolt.min,
      maxBatteryVolt: Number.isFinite(prev?.maxBatteryVolt)
        ? Math.max(prev.maxBatteryVolt, agg.batteryVolt.max)
        : agg.batteryVolt.max,

      // metadata
      bucketStartSec: agg.bucketStartSec,
      bucketStart: Timestamp.fromMillis(agg.bucketStartSec * 1000),

      updatedAt: FieldValue.serverTimestamp(),
    };

    // === AVG fields ===
    const c = next.count || 0;

    // làm tròn 
    const round = (x, d = 2) => {
      const p = Math.pow(10, d);
      return Math.round((x + Number.EPSILON) * p) / p;
    };

    next.avgSalinity     = c ? round(next.sumSalinity / c, 2) : null;
    next.avgTemperature  = c ? round(next.sumTemperature / c, 2) : null;
    next.avgPh           = c ? round(next.sumPh / c, 2) : null;
    next.avgBatteryPct   = c ? round(next.sumBatteryPct / c, 2) : null;
    next.avgBatteryVolt  = c ? round(next.sumBatteryVolt / c, 2) : null;

    tx.set(docRef, next, { merge: true });
  });
}

/**
 * Helper: parse number safely 
 */
function toFiniteOrNull(v) {
  const n = Number(v);
  return Number.isFinite(n) ? n : null;
}

/**
 * Helper: only update stats/latest if incoming record is newer
 * Prevent delayed uploads from overwriting the actual latest reading
 */
async function upsertLatestIfNewer(deviceRef, incoming) {
  const latestRef = deviceRef.collection("stats").doc("latest");

  await db.runTransaction(async (tx) => {
    const snap = await tx.get(latestRef);
    const prev = snap.exists ? snap.data() : null;

    const prevMeasuredAtSec = Number(prev?.measuredAtSec) || 0;
    const nextMeasuredAtSec = Number(incoming?.measuredAtSec) || 0;

    if (!snap.exists || nextMeasuredAtSec > prevMeasuredAtSec) {
      tx.set(
        latestRef,
        {
          ...incoming,
          measuredAt: Timestamp.fromMillis(nextMeasuredAtSec * 1000),
          updatedAt: FieldValue.serverTimestamp(),
        },
        { merge: true }
      );
    }
  });
}

/**
 * ============================================================
 * 1) HTTP ingestBatch: ESP32 -> Cloud Function -> Firestore (Prototype)
 * ============================================================
 */
exports.ingestBatch = onRequest({ region: "asia-southeast1" }, async (req, res) => {
  try {
    if (req.method !== "POST") {
      return res.status(405).json({ ok: false, error: "Method not allowed" });
    }

    const body = req.body || {};
    const deviceId = String(body.deviceId || "").trim();
    const key = String(body.key || "").trim();
    const records = body.records;

    if (!deviceId) return res.status(400).json({ ok: false, error: "Missing deviceId" });
    if (!key) return res.status(400).json({ ok: false, error: "Missing key" });
    if (key !== EXPECTED_DEVICE_KEY) return res.status(401).json({ ok: false, error: "Invalid key" });
    if (!Array.isArray(records) || records.length === 0) {
      return res.status(400).json({ ok: false, error: "Missing records[]" });
    }

    const deviceRef = db.collection("devices").doc(deviceId);

    await deviceRef.set(
      { lastSeen: FieldValue.serverTimestamp(), updatedAt: FieldValue.serverTimestamp() },
      { merge: true }
    );

    const batch = db.batch();
    let written = 0;
    let skipped = 0;
    let latest = null;

    const agg5m = new Map();
    const aggH = new Map();
    const aggD = new Map();

    for (const r of records) {
      const sal = Number(r.sal);
      const temp = Number(r.temp);
      const ph = Number(r.ph);

      const rawSal = Number(r.rawSal);
      const rawTemp = Number(r.rawTemp);
      const rawPh = Number(r.rawPh);
      const battPct = Number(r.battPct ?? r.batteryPct);
      const battVolt = Number(r.battVolt ?? r.batteryVolt);

      const measuredAtSec = Number(r.measuredAt);

      if (
        !Number.isFinite(sal) ||
        !Number.isFinite(temp) ||
        !Number.isFinite(ph) ||
        !Number.isFinite(rawSal) ||
        !Number.isFinite(rawTemp) ||
        !Number.isFinite(rawPh) ||
        !Number.isFinite(battPct) ||
        !Number.isFinite(battVolt) ||
        !Number.isFinite(measuredAtSec)
      ) {
        skipped++;
        continue;
      }

      const measuredAtTs = Timestamp.fromMillis(measuredAtSec * 1000);

      const { status, alerts } = evaluateStatus({ sal, temp, ph, batteryPct: battPct });

      const readingRef = deviceRef.collection("readings").doc();
      batch.set(readingRef, {
        salinity: sal,
        temperature: temp,
        ph: ph,

        rawSalinity: rawSal,
        rawTemperature: rawTemp,
        rawPh: rawPh,

        batteryPct: battPct,
        batteryVolt: battVolt,

        status,
        alerts,

        measuredAtSec,
        measuredAt: measuredAtTs,
        createdAt: FieldValue.serverTimestamp(),
        source: "http_ingest",
      });

      const b5 = min5Bucket(measuredAtSec);
      const bh = hourlyBucket(measuredAtSec);
      const bd = dailyBucket(measuredAtSec);

      addAgg(agg5m, b5.key, b5.bucketStartSec, sal, temp, ph, battPct, battVolt);
      addAgg(aggH,  bh.key, bh.bucketStartSec, sal, temp, ph, battPct, battVolt);
      addAgg(aggD,  bd.key, bd.bucketStartSec, sal, temp, ph, battPct, battVolt);

      written++;

      if (!latest || measuredAtSec > latest.measuredAtSec) {
        latest = { sal, temp, ph, rawSal, rawTemp, rawPh, battPct, battVolt, status, alerts, measuredAtSec };
      }
    }
    if (written === 0) return res.status(400).json({ ok: false, error: "No valid records to write" });

    await batch.commit();

    if (latest) {
      await upsertLatestIfNewer(deviceRef, {
        salinity: latest.sal,
        temperature: latest.temp,
        ph: latest.ph,

        rawSalinity: latest.rawSal,
        rawTemperature: latest.rawTemp,
        rawPh: latest.rawPh,

        batteryPct: latest.battPct,
        batteryVolt: latest.battVolt,

        status: latest.status,
        alerts: latest.alerts,

        measuredAtSec: latest.measuredAtSec,
        source: "http_ingest",
      });
    }

    for (const [k, a] of agg5m.entries()) await upsertAgg(deviceRef.collection("stats_5m").doc(k), a);
    for (const [k, a] of aggH.entries())  await upsertAgg(deviceRef.collection("stats_hourly").doc(k), a);
    for (const [k, a] of aggD.entries())  await upsertAgg(deviceRef.collection("stats_daily").doc(k), a);

    await deviceRef.collection("ingestLogs").add({
      source: "http_ingest",
      received: records.length,
      written,
      skipped,
      latestMeasuredAtSec: latest ? latest.measuredAtSec : null,
      latestStatus: latest ? latest.status : null,
      createdAt: FieldValue.serverTimestamp(),
    });

    return res.status(200).json({ ok: true, deviceId, received: records.length, written, skipped });
  } catch (err) {
    logger.error(err);
    return res.status(500).json({ ok: false, error: String(err?.message || err) });
  }
});

/**
 * ============================================================
 * 2) RTDB -> Firestore bridge 
 * RTDB: /data/{pushId}
 * fields: device_id, salinity, ambient_temp, ph, bat_p, bat_v, queue_id, measuredAtSec
 * ============================================================
 */
const REGION_RTDB = "asia-southeast1";
const RTDB_INSTANCE = "esp32-iot-demo-thesis-01";

exports.bridgeNewReadingsToFirestore = onValueCreated(
  {
    ref: "/data/{pushId}",     
    region: REGION_RTDB,
    instance: RTDB_INSTANCE,
  },
  async (event) => {
    try {
      const pushId = event.params.pushId;
      const x = event.data.val();
      if (!x) return;

      const deviceId = String(x.device_id || "").trim();
      if (!deviceId) return;

      // convert temp field name => ambient_temp 
      const sal = toFiniteOrNull(x.salinity);
      const temp = toFiniteOrNull(x.ambient_temp ?? x.temp);
      const ph = toFiniteOrNull(x.ph);

      const battPct = toFiniteOrNull(x.bat_p);
      const battVolt = toFiniteOrNull(x.bat_v);
      const queueId = toFiniteOrNull(x.queue_id);

      // use measuredAtSec from RTDB 
      const measuredAtSec = toFiniteOrNull(x.measuredAtSec) ?? Math.floor(Date.now() / 1000);
      const measuredAtTs = Timestamp.fromMillis(measuredAtSec * 1000);

      if (!Number.isFinite(sal) || !Number.isFinite(temp) || !Number.isFinite(ph)) {
        logger.warn("RTDB record missing core fields", { deviceId, pushId });
        return;
      }

      const battPctSafe = Number.isFinite(battPct) ? battPct : 0;
      const battVoltSafe = Number.isFinite(battVolt) ? battVolt : 0;

      const { status, alerts } = evaluateStatus({
        sal,
        temp,
        ph,
        batteryPct: battPctSafe,
      });

      const deviceRef = db.collection("devices").doc(deviceId);
      const readingRef = deviceRef.collection("readings").doc(pushId);

      await deviceRef.set(
        { lastSeen: FieldValue.serverTimestamp(), updatedAt: FieldValue.serverTimestamp() },
        { merge: true }
      );

      // idempotent write by docId = pushId
      await readingRef.set(
        {
          salinity: sal,
          temperature: temp, // ambient_temp -> temperature
          ph: ph,

          batteryPct: battPctSafe,
          batteryVolt: battVoltSafe,

          queueId: queueId,

          status,
          alerts,

          measuredAtSec: measuredAtSec,
          measuredAt: measuredAtTs,
          createdAt: FieldValue.serverTimestamp(),

          source: "rtdb_bridge",
        },
        { merge: false }
      );

      // update stats/latest only if this reading is newer
      await upsertLatestIfNewer(deviceRef, {
        salinity: sal,
        temperature: temp,
        ph: ph,

        batteryPct: battPctSafe,
        batteryVolt: battVoltSafe,

        status,
        alerts,

        measuredAtSec: measuredAtSec,
        source: "rtdb_bridge",
      });

      // stats buckets by measuredAtSec
      const b5 = min5Bucket(measuredAtSec);
      const bh = hourlyBucket(measuredAtSec);
      const bd = dailyBucket(measuredAtSec);

      const tmp = new Map();
      addAgg(tmp, "x", b5.bucketStartSec, sal, temp, ph, battPctSafe, battVoltSafe);
      const a = tmp.get("x");

      await upsertAgg(deviceRef.collection("stats_5m").doc(b5.key), a);
      await upsertAgg(deviceRef.collection("stats_hourly").doc(bh.key), a);
      await upsertAgg(deviceRef.collection("stats_daily").doc(bd.key), a);

      await deviceRef.collection("ingestLogs").add({
        source: "rtdb_bridge",
        written: 1,
        skipped: 0,
        pushId,
        createdAt: FieldValue.serverTimestamp(),
      });
    } catch (err) {
      logger.error("bridgeNewReadingsToFirestore error", err);
    }
  }
);
/**
 * ============================================================
 * 3) MIGRATE TOOL existing RTDB old data -> Firestore
 * Call:
 *  /migrateExistingReadings?limit=200
 *  /migrateExistingReadings?limit=200&startAfter=<pushId>
 * ============================================================
 */
exports.migrateExistingReadings = onRequest({ region: REGION_RTDB }, async (req, res) => {
  try {
    const limit = Math.min(parseInt(req.query.limit || "200", 10), 500);
    const startAfter = req.query.startAfter ? String(req.query.startAfter) : null;

    let q = rtdbThesis.ref("/data").orderByKey();
    if (startAfter) q = q.startAfter(startAfter);
    q = q.limitToFirst(limit);

    const snap = await q.get();
    const obj = snap.val();

    if (!obj) {
      return res.json({
        ok: true,
        migrated: 0,
        skipped: 0,
        nextStartAfter: null,
        done: true,
        message: "No more data under /data",
      });
    }

    const entries = Object.entries(obj);
    let migrated = 0;
    let skipped = 0;

    let batch = db.batch();
    let ops = 0;

    for (const [pushId, x] of entries) {
      if (!x) { skipped++; continue; }

      const deviceId = String(x.device_id || "").trim();
      if (!deviceId) { skipped++; continue; }

      const sal = toFiniteOrNull(x.salinity);
      const temp = toFiniteOrNull(x.ambient_temp ?? x.temp); // 
      const ph = toFiniteOrNull(x.ph);

      const battPct = toFiniteOrNull(x.bat_p);
      const battVolt = toFiniteOrNull(x.bat_v);
      const queueId = toFiniteOrNull(x.queue_id);

      const measuredAtSec = toFiniteOrNull(x.measuredAtSec) ?? Math.floor(Date.now() / 1000);
      const measuredAtTs = Timestamp.fromMillis(measuredAtSec * 1000);

      if (!Number.isFinite(sal) || !Number.isFinite(temp) || !Number.isFinite(ph)) {
        skipped++;
        continue;
      }

      const battPctSafe = Number.isFinite(battPct) ? battPct : 0;
      const battVoltSafe = Number.isFinite(battVolt) ? battVolt : 0;

      const { status, alerts } = evaluateStatus({
        sal,
        temp,
        ph,
        batteryPct: battPctSafe,
      });

      const deviceRef = db.collection("devices").doc(deviceId);
      const readingRef = deviceRef.collection("readings").doc(pushId);

      batch.set(
        readingRef,
        {
          salinity: sal,
          temperature: temp,
          ph: ph,

          batteryPct: battPctSafe,
          batteryVolt: battVoltSafe,

          queueId: queueId,

          status,
          alerts,

          measuredAtSec: measuredAtSec,
          measuredAt: measuredAtTs,
          createdAt: FieldValue.serverTimestamp(),

          source: "rtdb_migrate",
        },
        { merge: false }
      );

      ops++;
      migrated++;

      if (ops >= 450) {
        await batch.commit();
        batch = db.batch();
        ops = 0;
      }
    }

    if (ops > 0) await batch.commit();

    const lastKey = entries[entries.length - 1][0];

    return res.json({
      ok: true,
      migrated,
      skipped,
      nextStartAfter: lastKey,
      done: entries.length < limit,
      hint: "Call again with startAfter=nextStartAfter until done=true",
    });
  } catch (err) {
    logger.error(err);
    return res.status(500).json({ ok: false, error: String(err?.message || err) });
  }
});