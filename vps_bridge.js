/**
 * VPS Bridge Server (Node.js) - SSE Version
 * 
 * 1. WebSocket Server (Port 8765): Maintains connection with StackChan robots.
 * 2. MCP Server (SSE Port 3000): Exposes tools to control robots via HTTP Stream.
 */

const { Server } = require("@modelcontextprotocol/sdk/server/index.js");
const { SSEServerTransport } = require("@modelcontextprotocol/sdk/server/sse.js");
const { CallToolRequestSchema, ListToolsRequestSchema } = require("@modelcontextprotocol/sdk/types.js");
const WebSocket = require("ws");
const express = require("express");

// --- 1. WebSocket Server for Robots (Port 8765) ---
const wss = new WebSocket.Server({ port: 8765 });
const connectedRobots = new Map(); // Map<deviceId, WebSocket>

wss.on("connection", (ws) => {
    let deviceId = null;
    ws.on("message", (message) => {
        try {
            const data = JSON.parse(message);
            if (data.type === "register") {
                deviceId = data.id;
                connectedRobots.set(deviceId, ws);
                console.log(`[WSS] Robot registered: ${deviceId}`);
                ws.send(JSON.stringify({ type: "reg_ack", status: "ok" }));
            } else if (data.type === "ping") {
                ws.send(JSON.stringify({ type: "pong" }));
            }
        } catch (e) {
            console.error("[WSS] Failed to parse message from robot", e);
        }
    });

    ws.on("close", () => {
        if (deviceId) {
            connectedRobots.delete(deviceId);
            console.log(`[WSS] Robot disconnected: ${deviceId}`);
        }
    });
});

console.log("WebSocket Server for robots started on port 8765");

// --- 2. MCP Server Definition ---
const mcpServer = new Server(
    {
        name: "stackchan-control-bridge",
        version: "1.2.0",
    },
    {
        capabilities: {
            tools: {},
        },
    }
);

// Register Tools
mcpServer.setRequestHandler(ListToolsRequestSchema, async () => {
    return {
        tools: [
            {
                name: "mcp__stackchan_action",
                description: "Control a physical StackChan robot's movement and expression.",
                inputSchema: {
                    type: "object",
                    properties: {
                        robot_id: { type: "string", description: "The MAC address/ID of the robot." },
                        action: { 
                            type: "string", 
                            enum: ["rotate_head", "nod_head", "set_emotion"],
                            description: "The action to perform." 
                        },
                        value: { 
                            type: "integer", 
                            description: "The parameter value (e.g., degrees -90 to 90, or emotion index: 0-Happy, 1-Sad, 2-Angry, 3-Surprised, 4-Sleepy, 5-Neutral)." 
                        },
                    },
                    required: ["robot_id", "action", "value"],
                },
            },
            {
                name: "mcp__stackchan_update_config",
                description: "Update the robot's LLM gateway settings. Use this to switch the robot's personality or LLM provider.",
                inputSchema: {
                    type: "object",
                    properties: {
                        robot_id: { type: "string", description: "The MAC address/ID of the robot." },
                        url: { type: "string", description: "The new LLM Gateway Base URL." },
                        key: { type: "string", description: "The new API Key." },
                        model: { type: "string", description: "The new Model Name." },
                        use_custom: { type: "boolean", description: "Whether to use custom LLM settings." },
                        reboot: { type: "boolean", description: "Whether to reboot the robot immediately." }
                    },
                    required: ["robot_id"],
                },
            }
        ],
    };
});

// Handle Tool Calls
mcpServer.setRequestHandler(CallToolRequestSchema, async (request) => {
    const { name, arguments: args } = request.params;
    const robot_id = args.robot_id;
    const ws = connectedRobots.get(robot_id);

    if (!ws || ws.readyState !== WebSocket.OPEN) {
        return {
            content: [{ type: "text", text: `Error: Robot ${robot_id} is offline.` }],
            isError: true,
        };
    }

    if (name === "mcp__stackchan_action") {
        const { action, value } = args;
        ws.send(JSON.stringify({ type: "hw_control", action, value }));
        return {
            content: [{ type: "text", text: `Success: Action ${action}(${value}) sent to ${robot_id}.` }],
        };
    }

    if (name === "mcp__stackchan_update_config") {
        const { url, key, model, use_custom, reboot } = args;
        ws.send(JSON.stringify({
            type: "update_config",
            url, key, model,
            use_custom: use_custom ?? true,
            reboot: reboot ?? true
        }));
        return {
            content: [{ type: "text", text: `Configuration sent to robot ${robot_id}.` }],
        };
    }

    throw new Error(`Tool not found: ${name}`);
});

// --- 3. Express Server for MCP SSE (Port 3000) ---
const app = express();
let transport = null;

app.get("/sse", async (req, res) => {
    console.log("[MCP] New SSE connection established");
    transport = new SSEServerTransport("/messages", res);
    await mcpServer.connect(transport);
});

app.post("/messages", async (req, res) => {
    if (transport) {
        await transport.handlePostMessage(req, res);
    } else {
        res.status(404).end();
    }
});

const PORT = 3000;
app.listen(PORT, () => {
    console.log(`[MCP] Server (SSE) listening on http://0.0.0.0:${PORT}/sse`);
    console.log(`[MCP] Post Messages endpoint: http://0.0.0.0:${PORT}/messages`);
});
