import assert from "node:assert/strict";
import test from "node:test";
import { HomeDataService } from "./home.js";

function response(value: unknown): Response {
  return new Response(JSON.stringify(value), {
    status: 200,
    headers: { "content-type": "application/json" },
  });
}

test("normalizes and caches Toronto weather", async () => {
  let calls = 0;
  const fakeFetch = async () => {
    calls++;
    return response({
      current: { temperature_2m: 19.5, weather_code: 2 },
      daily: {
        temperature_2m_max: [23],
        temperature_2m_min: [14],
        precipitation_probability_max: [30],
      },
    });
  };
  const home = new HomeDataService(fakeFetch as typeof fetch, () => 1_800_000);
  assert.deepEqual(await home.weather(), {
    location: "TORONTO",
    temperatureC: 19.5,
    condition: "PARTLY CLOUDY",
    highC: 23,
    lowC: 14,
    precipitationPercent: 30,
    observedAt: 1800,
    source: "Open-Meteo",
  });
  await home.weather();
  assert.equal(calls, 1);
});

test("normalizes configured crypto symbols", async () => {
  const fakeFetch = async () => response({
    bitcoin: { usd: 100, usd_24h_change: 1.5 },
    ethereum: { usd: 20, usd_24h_change: -2 },
    solana: { usd: 3, usd_24h_change: 0.25 },
  });
  const home = new HomeDataService(fakeFetch as typeof fetch, () => 2_000_000);
  assert.deepEqual((await home.crypto()).quotes.map((q) => q.symbol), ["BTC", "ETH", "SOL"]);
});

test("normalizes the configured stock watchlist and labels it delayed", async () => {
  const fakeFetch = async (input: string | URL | Request) => {
    const symbol = String(input).match(/chart\/([^?]+)/)?.[1] ?? "UNKNOWN";
    return response({
      chart: { result: [{ meta: { regularMarketPrice: 123.45, regularMarketChangePercent: 1.25 } }] },
      symbol,
    });
  };
  const home = new HomeDataService(fakeFetch as typeof fetch, () => 3_000_000);
  const stocks = await home.stocks();
  assert.equal(stocks.status, "DELAYED");
  assert.deepEqual(stocks.quotes.map((quote) => quote.symbol), ["AAPL", "NVDA", "TSLA", "SPY"]);
});

test("rejects oversized upstream responses", async () => {
  const fakeFetch = async () => new Response("x".repeat(65 * 1024), { status: 200 });
  const home = new HomeDataService(fakeFetch as typeof fetch);
  await assert.rejects(home.weather(), /too large/);
});
