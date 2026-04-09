"use strict";

const fs = require("fs");
const http = require("http");
const path = require("path");
const WebSocket = require("ws");
const { SerialPort } = require("serialport");

const WEB_ROOT = path.join(__dirname, "..", "web");
const PORT = Number(process.env.PORT) || 8080;
const BAUD = Number(process.env.SERIAL_BAUD) || 9600;
/** Default matches Arduino Uno on macOS — override with SERIAL_PORT=com3 on Windows, etc. */
const SERIAL_PATH = process.env.SERIAL_PORT || "/dev/cu.usbmodem1301";

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".ico": "image/x-icon",
  ".png": "image/png",
  ".svg": "image/svg+xml",
};

function safeFilePath(urlPath) {
  const rel = urlPath === "/" ? "index.html" : urlPath.replace(/^\/+/, "");
  const full = path.resolve(path.join(WEB_ROOT, rel));
  const root = path.resolve(WEB_ROOT);
  if (!full.startsWith(root + path.sep) && full !== root) return null;
  return full;
}

const server = http.createServer((req, res) => {
  const urlPath = (req.url || "/").split("?")[0];
  const filePath = safeFilePath(urlPath);
  if (!filePath) {
    res.writeHead(403);
    res.end("Forbidden");
    return;
  }
  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404);
      res.end("Not found");
      return;
    }
    const ext = path.extname(filePath).toLowerCase();
    res.setHeader("Content-Type", MIME[ext] || "application/octet-stream");
    res.end(data);
  });
});

const wss = new WebSocket.Server({ noServer: true });
const clients = new Set();

function broadcastLine(line) {
  for (const c of clients) {
    if (c.readyState === WebSocket.OPEN) c.send(line);
  }
}

let serialPort = null;
let serialBuf = "";

function openSerial() {
  serialPort = new SerialPort({
    path: SERIAL_PATH,
    baudRate: BAUD,
    autoOpen: false,
  });

  serialPort.open((err) => {
    if (err) {
      console.error("[serial] open failed:", err.message);
      broadcastLine("BRIDGE:ERROR:" + err.message);
      return;
    }
    console.log("[serial] open", SERIAL_PATH, BAUD);
    broadcastLine("BRIDGE:READY");
  });

  serialPort.on("data", (chunk) => {
    serialBuf += chunk.toString("utf8");
    let i;
    while ((i = serialBuf.indexOf("\n")) >= 0) {
      const line = serialBuf.slice(0, i).replace(/\r$/, "");
      serialBuf = serialBuf.slice(i + 1);
      if (line.length) broadcastLine(line);
    }
  });

  serialPort.on("error", (e) => {
    console.error("[serial]", e.message);
    broadcastLine("BRIDGE:ERROR:" + e.message);
  });

  serialPort.on("close", () => {
    broadcastLine("BRIDGE:SERIAL_CLOSED");
  });
}

wss.on("connection", (socket) => {
  clients.add(socket);
  socket.on("message", (data) => {
    if (!serialPort || !serialPort.isOpen) return;
    const text = Buffer.isBuffer(data) ? data.toString("utf8") : String(data);
    const out = text.endsWith("\n") ? text : text + "\n";
    serialPort.write(out, (err) => {
      if (err) console.error("[serial] write:", err.message);
    });
  });
  socket.on("close", () => clients.delete(socket));
});

server.on("upgrade", (req, socket, head) => {
  const p = (req.url || "").split("?")[0];
  if (p === "/ws") {
    wss.handleUpgrade(req, socket, head, (ws) => {
      wss.emit("connection", ws, req);
    });
  } else {
    socket.destroy();
  }
});

server.listen(PORT, () => {
  console.log("Web UI:  http://localhost:" + PORT);
  console.log("WebSocket path: /ws — Arduino on " + SERIAL_PATH + " @ " + BAUD);
  openSerial();
});
