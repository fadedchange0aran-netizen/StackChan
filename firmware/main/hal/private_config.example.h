#pragma once

/**
 * @brief Private configuration template.
 * Copy to "private_config.h" and fill in your values.
 */

// VPS Bridge Configuration (Robot Control)
#define PRIVATE_BRIDGE_URL      "wss://your-bridge.example.com/robot-wss"
#define PRIVATE_BRIDGE_TOKEN    "YOUR_TOKEN_HERE"

// Mode Switch Configuration (Xiaozhi Official WS compat)
#define PRIVATE_XIAOZHI_WS_URL  "wss://your-bridge.example.com/ws"

// Custom Gateway Configuration (secret_logic)
#define PRIVATE_CUSTOM_URL      "https://your-gateway.example.com/v1"
#define PRIVATE_CUSTOM_KEY      "YOUR_KEY_HERE"

// Handshake / OTA Logic
#define PRIVATE_HANDSHAKE_TOKEN "your-handshake-token"
#define PRIVATE_DOMAIN_HINT    "your-bridge.example.com"
