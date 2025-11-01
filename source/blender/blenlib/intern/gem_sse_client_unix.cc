/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * SSE Client - Unix/Linux/macOS Implementation (libcurl)
 *
 * Platform-specific SSE client using libcurl for Unix/Linux/macOS.
 * Runs HTTP GET request on background thread and parses SSE format.
 *
 * SSE Format:
 * - "data: {json}\n\n" - Data line followed by empty line
 * - "event: type\ndata: {json}\n\n" - Optional event type
 * - "id: 123\n" - Optional event ID
 * - ": comment\n" - Comments (ignored)
 * - Empty line triggers event dispatch
 */

#ifndef _WIN32

#include "BLI_gem_sse_client.hh"

#include <curl/curl.h>
#include <atomic>
#include <mutex>
#include <sstream>
#include <thread>

namespace blender::sse {

/**
 * Platform-specific implementation for Unix/Linux/macOS.
 */
struct SseClient::Impl {
  /** Background thread for HTTP connection */
  std::thread worker_thread;

  /** Atomic flag for connection state */
  std::atomic<bool> connected{false};
  std::atomic<bool> should_stop{false};

  /** Callbacks */
  SseEventCallback event_callback;
  SseErrorCallback error_callback;

  /** Connection URL */
  std::string url;

  /** Mutex for thread synchronization */
  std::mutex mutex;

  /** Line buffer for SSE parsing */
  std::string line_buffer;

  /** Current event being parsed */
  SseEvent current_event;
  bool has_data = false;

  /**
   * Parse SSE format and extract events.
   * SSE format: "data: ...\n\n" or "event: type\ndata: ...\n\n"
   */
  void parse_sse_line(const std::string &line)
  {
    if (line.empty()) {
      /* Empty line = event complete, dispatch it */
      if (has_data && event_callback) {
        /* Parse JSON data to extract fields */
        extract_event_fields(current_event);
        event_callback(current_event);
      }
      /* Reset for next event */
      current_event = SseEvent();
      has_data = false;
    }
    else if (line[0] == ':') {
      /* Comment line - ignore (heartbeat comments) */
      return;
    }
    else if (line.rfind("data: ", 0) == 0) {
      /* Data line */
      if (!current_event.data.empty()) {
        current_event.data += "\n";
      }
      current_event.data += line.substr(6);
      has_data = true;
    }
    else if (line.rfind("event: ", 0) == 0) {
      /* Event type line */
      current_event.event_type = line.substr(7);
    }
    else if (line.rfind("id: ", 0) == 0) {
      /* Event ID line */
      current_event.id = line.substr(4);
    }
  }

  /**
   * Extract fields from JSON data (simple string parsing, no full JSON parser).
   * Looks for: "type", "timestamp", "requiresReexecution"
   */
  void extract_event_fields(SseEvent &event)
  {
    const std::string &data = event.data;

    /* Extract type if not set by event: line */
    if (event.event_type.empty()) {
      size_t type_pos = data.find("\"type\":");
      if (type_pos != std::string::npos) {
        size_t start = data.find("\"", type_pos + 7);
        if (start != std::string::npos) {
          size_t end = data.find("\"", start + 1);
          if (end != std::string::npos) {
            event.event_type = data.substr(start + 1, end - start - 1);
          }
        }
      }
    }

    /* Extract timestamp */
    size_t ts_pos = data.find("\"timestamp\":");
    if (ts_pos != std::string::npos) {
      size_t start = data.find("\"", ts_pos + 12);
      if (start != std::string::npos) {
        size_t end = data.find("\"", start + 1);
        if (end != std::string::npos) {
          event.timestamp = data.substr(start + 1, end - start - 1);
        }
      }
    }

    /* Extract requiresReexecution (bool) */
    size_t reexec_pos = data.find("\"requiresReexecution\":");
    if (reexec_pos != std::string::npos) {
      size_t true_pos = data.find("true", reexec_pos + 22);
      event.requires_reexecution = (true_pos != std::string::npos &&
                                     true_pos < reexec_pos + 30);
    }
  }

  /**
   * libcurl write callback - called when data is received.
   * Processes data line by line and parses SSE format.
   */
  static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
  {
    Impl *impl = static_cast<Impl *>(userdata);
    size_t total_size = size * nmemb;

    /* Check if we should stop */
    if (impl->should_stop) {
      return 0;  /* Return 0 to abort transfer */
    }

    /* Process data line by line */
    for (size_t i = 0; i < total_size; i++) {
      if (ptr[i] == '\n') {
        /* Line complete */
        impl->parse_sse_line(impl->line_buffer);
        impl->line_buffer.clear();
      }
      else if (ptr[i] != '\r') {
        /* Add to line buffer (skip \r) */
        impl->line_buffer += ptr[i];
      }
    }

    return total_size;
  }

  /**
   * Worker thread function - manages HTTP connection using libcurl.
   */
  void worker_func()
  {
    /* Initialize libcurl */
    CURL *curl = curl_easy_init();
    if (!curl) {
      if (error_callback) {
        error_callback("Failed to initialize libcurl");
      }
      return;
    }

    /* Set options */
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);  /* No timeout */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);  /* 10 second connect timeout */

    /* Set Accept header for SSE */
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    headers = curl_slist_append(headers, "Cache-Control: no-cache");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    /* Perform request (blocking until connection ends) */
    connected = true;
    CURLcode res = curl_easy_perform(curl);

    /* Check result */
    if (res != CURLE_OK) {
      if (error_callback && !should_stop) {
        std::string error = "libcurl error: ";
        error += curl_easy_strerror(res);
        error_callback(error);
      }
    }
    else {
      /* Check HTTP status code */
      long response_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

      if (response_code != 200) {
        if (error_callback) {
          std::string error = "HTTP error: " + std::to_string(response_code);
          error_callback(error);
        }
      }
    }

    /* Cleanup */
    connected = false;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
  }
};

/* -------------------------------------------------------------------- */
/** \name Public API Implementation
 * \{ */

SseClient::SseClient() : impl_(std::make_unique<Impl>())
{
  /* Initialize libcurl globally (thread-safe after curl 7.84.0) */
  static bool curl_initialized = false;
  static std::mutex init_mutex;

  if (!curl_initialized) {
    std::lock_guard<std::mutex> lock(init_mutex);
    if (!curl_initialized) {
      curl_global_init(CURL_GLOBAL_DEFAULT);
      curl_initialized = true;
    }
  }
}

SseClient::~SseClient()
{
  disconnect();
}

std::unique_ptr<SseClient> SseClient::connect(const std::string &url,
                                                SseEventCallback event_callback,
                                                SseErrorCallback error_callback)
{
  auto client = std::unique_ptr<SseClient>(new SseClient());

  client->impl_->url = url;
  client->impl_->event_callback = event_callback;
  client->impl_->error_callback = error_callback;

  /* Start worker thread */
  client->impl_->worker_thread = std::thread([impl = client->impl_.get()]() {
    impl->worker_func();
  });

  return client;
}

void SseClient::disconnect()
{
  if (impl_) {
    impl_->should_stop = true;
    impl_->connected = false;

    /* Wait for worker thread to finish (with timeout) */
    if (impl_->worker_thread.joinable()) {
      impl_->worker_thread.join();
    }
  }
}

bool SseClient::is_connected() const
{
  return impl_ && impl_->connected;
}

/** \} */

}  // namespace blender::sse

#endif  /* !_WIN32 */
