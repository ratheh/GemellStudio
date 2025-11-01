/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Server-Sent Events (SSE) Client for GemellStudio
 *
 * Provides non-blocking SSE connection to external services for real-time update notifications.
 * Used by external geometry nodes to subscribe to change events from external node services.
 *
 * Architecture:
 * - Background thread manages HTTP connection and SSE parsing
 * - Event callbacks delivered to main thread via flag+timer mechanism
 * - Platform-specific implementations (WinHTTP on Windows, libcurl on Unix/Linux/macOS)
 * - Thread-safe flag setting for event notification
 *
 * SSE Protocol (from Protocol-External-Geometry-Nodes.md):
 * - Event format: "data: {json}\n\n" or "event: type\ndata: {json}\n\n"
 * - Heartbeat every 20 seconds: ": heartbeat\n\n"
 * - Empty line triggers event dispatch
 *
 * Example Usage:
 * ```cpp
 * auto client = SseClient::connect(
 *     "http://localhost:8567/api/v1/geonodes/nodes/node123/events",
 *     [](const SseEvent &event) {
 *       if (event.event_type == "nodeDataChanged") {
 *         // Set flag for main thread to invalidate node
 *       }
 *     },
 *     [](const std::string &error) {
 *       // Log error
 *     });
 *
 * // Later: disconnect when node is deleted
 * client->disconnect();
 * ```
 */

#include <functional>
#include <memory>
#include <string>

namespace blender::sse {

/**
 * SSE event structure.
 * Represents a single Server-Sent Event received from the stream.
 */
struct SseEvent {
  /** Event type (e.g., "nodeDataChanged", "heartbeat", "connected") */
  std::string event_type;

  /** Event data (usually JSON string) */
  std::string data;

  /** Optional event ID (for resuming connections) */
  std::string id;

  /** Timestamp extracted from JSON data (if present) */
  std::string timestamp;

  /** Whether this event requires node re-execution (extracted from JSON data) */
  bool requires_reexecution;

  SseEvent() : requires_reexecution(false) {}
};

/**
 * Callback for SSE events.
 * Called from background thread when events are received and parsed.
 * Should be fast and thread-safe (just set flags, don't call Blender API).
 */
using SseEventCallback = std::function<void(const SseEvent &event)>;

/**
 * Callback for SSE errors.
 * Called when connection fails or is lost.
 */
using SseErrorCallback = std::function<void(const std::string &error)>;

/**
 * SSE client handle.
 * Manages background thread connection to SSE endpoint.
 * Non-blocking - events delivered via callbacks on background thread.
 */
class SseClient {
 public:
  /**
   * Connect to SSE endpoint and start receiving events.
   * Non-blocking - returns immediately, events delivered via callback.
   *
   * \param url: Full URL to SSE endpoint (e.g., "http://localhost:8567/api/v1/geonodes/nodes/abc/events")
   * \param event_callback: Called for each received event (runs on background thread)
   * \param error_callback: Called on connection errors (runs on background thread)
   * \return: SseClient instance, or nullptr if connection fails immediately
   *
   * Thread Safety: Callbacks run on background thread, must be thread-safe.
   */
  static std::unique_ptr<SseClient> connect(const std::string &url,
                                             SseEventCallback event_callback,
                                             SseErrorCallback error_callback);

  /**
   * Disconnect and stop receiving events.
   * Blocks until background thread terminates (typically < 100ms).
   */
  void disconnect();

  /**
   * Check if still connected.
   * Thread-safe.
   */
  bool is_connected() const;

  /**
   * Destructor - ensures clean disconnect.
   */
  ~SseClient();

 private:
  /** Platform-specific implementation (opaque pointer) */
  struct Impl;
  std::unique_ptr<Impl> impl_;

  /** Private constructor - use connect() factory method */
  SseClient();

  /** Prevent copying */
  SseClient(const SseClient &) = delete;
  SseClient &operator=(const SseClient &) = delete;
};

}  // namespace blender::sse
