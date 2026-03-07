export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const forceRefresh = url.searchParams.get("refresh") === "1";

    // -------------------------------------------------
    // Compute "stats day" in America/New_York @ 1 AM
    // -------------------------------------------------
    const now = new Date();

    const nyParts = new Intl.DateTimeFormat("en-US", {
      timeZone: "America/New_York",
      year: "numeric",
      month: "2-digit",
      day: "2-digit",
      hour: "2-digit",
      hour12: false,
    }).formatToParts(now);

    const get = (t) => nyParts.find(p => p.type === t)?.value;

    let year = get("year");
    let month = get("month");
    let day = get("day");
    const hour = parseInt(get("hour"), 10);

    // Before 1 AM → still count as previous day
    if (hour < 1) {
      const d = new Date(
        new Date(`${year}-${month}-${day}T12:00:00Z`).getTime() - 86400000
      );
      year = d.getUTCFullYear();
      month = String(d.getUTCMonth() + 1).padStart(2, "0");
      day = String(d.getUTCDate()).padStart(2, "0");
    }

    const statsDay = `${year}-${month}-${day}`;

    // -------------------------------------------------
    // Cache handling
    // -------------------------------------------------
    const cache = caches.default;
    const cacheKey = new Request(
      `${url.origin}${url.pathname}?day=${statsDay}`,
      request
    );

    if (!forceRefresh) {
      const cached = await cache.match(cacheKey);
      if (cached) {
        return cached;
      }
    }

    // -------------------------------------------------
    // Config
    // -------------------------------------------------
    const ICS_URL =
      "https://calendar.google.com/calendar/ical/092fc62584aa9e82bc84bc0bc4dfaec744603f10c4f647eba04e67c5994c3f76%40group.calendar.google.com/public/basic.ics";

    const API_BASE = "https://divelogs.de/api";

    if (!env.DIVEL0GS_USER || !env.DIVEL0GS_PASS) {
      return new Response(
        JSON.stringify({ error: "Missing env vars" }, null, 2),
        { status: 500, headers: { "Content-Type": "application/json" } }
      );
    }

    // -------------------------------------------------
    // Helpers
    // -------------------------------------------------
    const formatDuration = (sec) => {
      const min = Math.floor(sec / 60);
      if (min < 60) return `${min} min`;
      const h = Math.floor(min / 60);
      const m = min % 60;
      return m === 0 ? `${h} h` : `${h} h ${m} min`;
    };

    const metersToFeet = (m) => Math.round(m * 3.28084);

    // -------------------------------------------------
    // Fetch ICS + login + dives
    // -------------------------------------------------
    const icsPromise = fetch(ICS_URL);

    const form = new FormData();
    form.append("user", env.DIVEL0GS_USER);
    form.append("pass", env.DIVEL0GS_PASS);

    const loginResp = await fetch(`${API_BASE}/login`, {
      method: "POST",
      body: form,
    });

    const { bearer_token: token } = await loginResp.json();

    const divesResp = await fetch(`${API_BASE}/dives`, {
      headers: { Authorization: `Bearer ${token}` },
    });

    const [icsResp, dives] = await Promise.all([
      icsPromise,
      divesResp.json(),
    ]);

    // -------------------------------------------------
    // Compute stats
    // -------------------------------------------------
    let totalDuration = 0;
    let deepestMeters = 0;

    for (const d of dives) {
      if (Number.isFinite(d.duration)) totalDuration += d.duration;
      if (Number.isFinite(d.maxdepth) && d.maxdepth > deepestMeters)
        deepestMeters = d.maxdepth;
    }

    const totalDives = dives.length;
    const totalMinutesUnderwater = formatDuration(totalDuration);
    const deepestDive = `${metersToFeet(deepestMeters)} ft`;

    // -------------------------------------------------
    // Parse ICS
    // -------------------------------------------------
    const icsText = await icsResp.text();

    let nextDive = "";
    let nextDiveStart = null;
    let daysUntil = null;

    const s = icsText.indexOf("SUMMARY:");
    if (s >= 0) {
      nextDive = icsText.substring(s + 8, icsText.indexOf("\n", s)).trim();
    }

    const d = icsText.indexOf("DTSTART:");
    if (d >= 0) {
      const dt = icsText.substring(d + 8, d + 24);
      const date = new Date(Date.UTC(
        +dt.slice(0, 4),
        +dt.slice(4, 6) - 1,
        +dt.slice(6, 8),
        +dt.slice(9, 11),
        +dt.slice(11, 13),
        +dt.slice(13, 15)
      ));
      nextDiveStart = Math.floor(date.getTime() / 1000);
      daysUntil = Math.floor(
        (nextDiveStart - Date.now() / 1000) / 86400
      );
    }

    // -------------------------------------------------
    // Response + cache
    // -------------------------------------------------
    const response = new Response(
      JSON.stringify(
        {
          statsDay,
          nextDive,
          nextDiveStart,
          daysUntil,
          totalDives,
          totalMinutesUnderwater,
          deepestDive,
        },
        null,
        2
      ),
      {
        headers: {
          "Content-Type": "application/json",
          "Cache-Control": "public, max-age=31536000",
          "Access-Control-Allow-Origin": "*",
        },
      }
    );

    ctx.waitUntil(cache.put(cacheKey, response.clone()));
    return response;
  },
};
