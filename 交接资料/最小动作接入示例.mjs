import http from "node:http";

const deviceUrl = (process.env.LAMP_DEVICE_URL || "").replace(/\/+$/, "");
const listenPort = Number(process.env.PORT || 8787);
if (!deviceUrl) {
  console.error("Set LAMP_DEVICE_URL, for example http://192.168.1.50:8080");
  process.exit(1);
}

let token = "";

async function readDevicePage() {
  const response = await fetch(`${deviceUrl}/`);
  if (!response.ok) throw new Error(`Device page failed: HTTP ${response.status}`);
  const html = await response.text();
  const match = html.match(/const token='([0-9a-f]{32})'/);
  if (!match) throw new Error("Device page did not contain a 32-character session token");
  token = match[1];
}

async function deviceRequest(path, options = {}, retried = false) {
  if (!token) await readDevicePage();
  const headers = {
    "X-Lamp-Token": token,
    ...(options.body ? { "Content-Type": "application/json" } : {}),
    ...(options.headers || {}),
  };
  const response = await fetch(`${deviceUrl}${path}`, { ...options, headers });
  if (response.status === 403 && !retried) {
    await readDevicePage();
    return deviceRequest(path, options, true);
  }
  const text = await response.text();
  let data;
  try {
    data = JSON.parse(text);
  } catch {
    throw new Error(`Device returned non-JSON HTTP ${response.status}`);
  }
  if (!response.ok || data.ok === false || data.isError === true) {
    const message = data.error?.message || data.content?.[0]?.text || `HTTP ${response.status}`;
    const error = new Error(message);
    error.status = response.status;
    error.data = data;
    throw error;
  }
  return data;
}

function json(response, status, value) {
  response.writeHead(status, {
    "Content-Type": "application/json; charset=utf-8",
    "Cache-Control": "no-store",
    "Access-Control-Allow-Origin": "http://localhost:8787",
  });
  response.end(JSON.stringify(value));
}

function errorResponse(response, error) {
  json(response, error.status || 502, {
    ok: false,
    error: { message: error.message },
  });
}

async function body(request) {
  const chunks = [];
  for await (const chunk of request) chunks.push(chunk);
  const text = Buffer.concat(chunks).toString("utf8");
  if (text.length > 4096) throw Object.assign(new Error("Request body too large"), { status: 413 });
  return text ? JSON.parse(text) : {};
}

const server = http.createServer(async (request, response) => {
  try {
    if (request.method === "OPTIONS") {
      response.writeHead(204, {
        "Access-Control-Allow-Origin": "http://localhost:8787",
        "Access-Control-Allow-Headers": "Content-Type",
        "Access-Control-Allow-Methods": "GET,POST,OPTIONS",
      });
      response.end();
      return;
    }

    if (request.method === "GET" && request.url === "/api/state") {
      json(response, 200, await deviceRequest("/api/lamp/state"));
      return;
    }
    if (request.method === "GET" && request.url === "/api/actions") {
      json(response, 200, await deviceRequest("/api/lamp/delivery"));
      return;
    }
    if (request.method === "POST" && request.url === "/api/play") {
      const value = await body(request);
      if (typeof value.id !== "string" || !/^[A-Za-z0-9_-]{1,40}$/.test(value.id)) {
        throw Object.assign(new Error("id must be a valid action ID"), { status: 400 });
      }
      const endMode = value.end_mode || "return_to_reference";
      if (!["return_to_reference", "hold_at_end"].includes(endMode)) {
        throw Object.assign(new Error("Invalid end_mode"), { status: 400 });
      }
      const rpc = await deviceRequest("/api/mcp/call", {
        method: "POST",
        body: JSON.stringify({
          name: "self.lamp.play_user_actions",
          arguments: {
            ids_json: JSON.stringify([value.id]),
            end_mode: endMode,
          },
        }),
      });
      const toolText = rpc.content?.[0]?.text;
      json(response, 200, toolText ? JSON.parse(toolText) : rpc);
      return;
    }
    const job = request.url.match(/^\/api\/jobs\/([^/?]+)$/);
    if (request.method === "GET" && job) {
      json(response, 200, await deviceRequest(`/api/lamp/delivery/jobs/${job[1]}`));
      return;
    }
    const task = request.url.match(/^\/api\/tasks\/([^/?]+)$/);
    if (request.method === "GET" && task) {
      json(response, 200, await deviceRequest(`/api/lamp/tasks/${task[1]}`));
      return;
    }
    json(response, 404, { ok: false, error: { message: "Unknown proxy route" } });
  } catch (error) {
    errorResponse(response, error);
  }
});

server.listen(listenPort, "127.0.0.1", () => {
  console.log(`Course proxy: http://localhost:${listenPort}`);
  console.log(`ESP32 device: ${deviceUrl}`);
});
