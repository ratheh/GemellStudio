/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * SSE Client - Windows Implementation (WinHTTP)
 *
 * Platform-specific SSE client using WinHTTP for Windows.
 * Runs HTTP GET request on background thread and parses SSE format.
 *
 * SSE Format:
 * - "data: {json}\n\n" - Data line followed by empty line
 * - "event: type\ndata: {json}\n\n" - Optional event type
 * - "id: 123\n" - Optional event ID
 * - ": comment\n" - Comments (ignored)
 * - Empty line triggers event dispatch
 *
 * Note: This is in blenlib (low-level library) so we can't use CLG logging here.
 * Logging happens at higher levels via the event_callback.
 */

#ifdef _WIN32

#include "BLI_gem_sse_client.hh"

#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace blender::sse {

/**
 * Platform-specific implementation for Windows.
 */
struct SseClient::Impl {
  /** Background thread for HTTP connection */
  std::thread worker_thread;

  /** Atomic flags for connection and thread state */
  std::atomic<bool> connected{false};
  std::atomic<bool> should_stop{false};
  std::atomic<bool> thread_finished{false};  /* Set by worker before exit */

  /** Callbacks */
  SseEventCallback event_callback;
  SseErrorCallback error_callback;

  /** Connection URL */
  std::string url;

  /** Mutex for thread synchronization */
  std::mutex mutex;

  /** WinHTTP handles (accessed from both worker and main thread, protected by mutex) */
  HINTERNET hSession{nullptr};
  HINTERNET hConnect{nullptr};
  HINTERNET hRequest{nullptr};

  /**
   * Parse SSE format and extract events.
   * SSE format: "data: ...\n\n" or "event: type\ndata: ...\n\n"
   */
  void parse_sse_line(const std::string &line, SseEvent &current_event, bool &has_data)
  {
    if (line.empty()) {
      /* Empty line = event complete, dispatch it */
      if (has_data && event_callback) {
        /* Parse JSON data to extract fields */
        extract_event_fields(current_event);

        /* Dispatch event to callback (logging happens at higher level) */
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
   * Worker thread function - manages HTTP connection and SSE parsing.
   */
  void worker_func()
  {
    /* Parse URL */
    std::wstring wurl = std::wstring(url.begin(), url.end());

    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwHostNameLength = (DWORD)-1;
    urlComp.dwUrlPathLength = (DWORD)-1;

    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.length(), 0, &urlComp)) {
      if (error_callback) {
        error_callback("Failed to parse URL");
      }
      return;
    }

    std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
    std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
    INTERNET_PORT port = urlComp.nPort;

    /* Initialize WinHTTP */
    {
      std::lock_guard<std::mutex> lock(mutex);
      hSession = WinHttpOpen(L"GemellStudio/1.0",
                             WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS,
                             0);
    }

    if (!hSession) {
      if (error_callback) {
        error_callback("WinHTTP session creation failed");
      }
      return;
    }

    /* Set timeouts (all in milliseconds) */
    /* Resolve timeout: 30 seconds */
    DWORD resolveTimeout = 30000;
    WinHttpSetOption(hSession, WINHTTP_OPTION_RESOLVE_TIMEOUT, &resolveTimeout, sizeof(resolveTimeout));

    /* Connect timeout: 30 seconds */
    DWORD connectTimeout = 30000;
    WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &connectTimeout, sizeof(connectTimeout));

    /* Send timeout: 30 seconds */
    DWORD sendTimeout = 30000;
    WinHttpSetOption(hSession, WINHTTP_OPTION_SEND_TIMEOUT, &sendTimeout, sizeof(sendTimeout));

    /* Receive timeout: 60 seconds (SSE can be idle between events) */
    DWORD recvTimeout = 60000;
    WinHttpSetOption(hSession, WINHTTP_OPTION_RECEIVE_TIMEOUT, &recvTimeout, sizeof(recvTimeout));

    /* Connect */
    {
      std::lock_guard<std::mutex> lock(mutex);
      hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    }
    if (!hConnect) {
      WinHttpCloseHandle(hSession);
      if (error_callback) {
        error_callback("WinHTTP connection failed");
      }
      return;
    }

    /* Open request (GET) */
    {
      std::lock_guard<std::mutex> lock(mutex);
      hRequest = WinHttpOpenRequest(hConnect,
                                     L"GET",
                                     path.c_str(),
                                     NULL,
                                     WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     0);
    }

    if (!hRequest) {
      WinHttpCloseHandle(hConnect);
      WinHttpCloseHandle(hSession);
      if (error_callback) {
        error_callback("WinHTTP request creation failed");
      }
      return;
    }

    /* Set Accept header for SSE */
    WinHttpAddRequestHeaders(hRequest,
                              L"Accept: text/event-stream",
                              (DWORD)-1L,
                              WINHTTP_ADDREQ_FLAG_ADD);

    /* Send request */
    if (!WinHttpSendRequest(hRequest,
                             WINHTTP_NO_ADDITIONAL_HEADERS,
                             0,
                             WINHTTP_NO_REQUEST_DATA,
                             0,
                             0,
                             0)) {
      WinHttpCloseHandle(hRequest);
      WinHttpCloseHandle(hConnect);
      WinHttpCloseHandle(hSession);
      if (error_callback) {
        error_callback("WinHTTP send request failed");
      }
      return;
    }

    /* Receive response */
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
      WinHttpCloseHandle(hRequest);
      WinHttpCloseHandle(hConnect);
      WinHttpCloseHandle(hSession);
      if (error_callback) {
        error_callback("WinHTTP receive response failed");
      }
      return;
    }

    /* Check status code */
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
                         WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         WINHTTP_HEADER_NAME_BY_INDEX,
                         &statusCode,
                         &statusCodeSize,
                         WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 200) {
      WinHttpCloseHandle(hRequest);
      WinHttpCloseHandle(hConnect);
      WinHttpCloseHandle(hSession);
      if (error_callback) {
        std::string error = "HTTP error: " + std::to_string(statusCode);
        error_callback(error);
      }
      return;
    }

    /* Connection established */
    connected = true;

    /* Read data in chunks and parse SSE
     *
     * SSE Streaming Strategy:
     * - Use WinHttpQueryDataAvailable to check for data without blocking
     * - Sleep briefly if no data available (allows checking should_stop flag)
     * - This prevents blocking in WinHttpReadData which would delay shutdown
     * - SSE server flushes after each event, so data arrives incrementally
     */
    const DWORD BUFFER_SIZE = 4096;
    char buffer[BUFFER_SIZE];
    std::string line_buffer;
    SseEvent current_event;
    bool has_data = false;

    while (!should_stop && connected) {
      /* Check if data is available without blocking */
      DWORD bytesAvailable = 0;
      if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
        /* Query failed - connection likely closed */
        break;
      }

      if (bytesAvailable == 0) {
        /* No data available yet - sleep briefly to avoid busy-wait */
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        continue;
      }

      /* Read available data (won't block since we know data exists) */
      DWORD bytesToRead = (bytesAvailable < BUFFER_SIZE - 1) ? bytesAvailable : (BUFFER_SIZE - 1);
      DWORD bytesRead = 0;

      if (WinHttpReadData(hRequest, buffer, bytesToRead, &bytesRead)) {
        if (bytesRead == 0) {
          /* End of stream */
          break;
        }

        /* Null-terminate */
        buffer[bytesRead] = '\0';

        /* Process data line by line */
        for (DWORD i = 0; i < bytesRead; i++) {
          if (buffer[i] == '\n') {
            /* Line complete - dispatch event immediately */
            parse_sse_line(line_buffer, current_event, has_data);
            line_buffer.clear();
          }
          else if (buffer[i] != '\r') {
            /* Add to line buffer (skip \r) */
            line_buffer += buffer[i];
          }
        }
      }
      else {
        /* Read error */
        break;
      }
    }

    /* Cleanup - close handles if they haven't been closed by disconnect() */
    connected = false;

    /* Close handles and clear members (protected by mutex to avoid double-free) */
    {
      std::lock_guard<std::mutex> lock(mutex);
      /* Check each handle - may be null or INVALID_HANDLE_VALUE if disconnect() already closed them */
      if (hRequest && hRequest != INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(hRequest);
        hRequest = nullptr;
      }
      if (hConnect && hConnect != INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(hConnect);
        hConnect = nullptr;
      }
      if (hSession && hSession != INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(hSession);
        hSession = nullptr;
      }
    }

    /* Mark thread as finished before exiting */
    thread_finished = true;
  }
};

/* -------------------------------------------------------------------- */
/** \name Public API Implementation
 * \{ */

SseClient::SseClient() : impl_(std::make_unique<Impl>())
{
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

    /* Close WinHTTP handles to force worker thread to exit immediately.
     * Closing the request handle will abort any blocking WinHTTP calls
     * (WinHttpQueryDataAvailable, WinHttpReadData, etc.) and cause them
     * to return immediately with an error. This prevents the 60-second
     * receive timeout from causing shutdown hangs. */
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      /* Check for valid handle (not null and not INVALID_HANDLE_VALUE) */
      if (impl_->hRequest && impl_->hRequest != INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(impl_->hRequest);
        impl_->hRequest = nullptr;
      }
      if (impl_->hConnect && impl_->hConnect != INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(impl_->hConnect);
        impl_->hConnect = nullptr;
      }
      if (impl_->hSession && impl_->hSession != INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(impl_->hSession);
        impl_->hSession = nullptr;
      }
    }

    /* Wait for worker thread to finish (should be quick now that handles are closed) */
    if (impl_->worker_thread.joinable()) {
      auto start = std::chrono::steady_clock::now();
      const auto timeout = std::chrono::milliseconds(30000);  /* 30 second timeout for slow PCs and obvious failure detection */

      while (impl_->worker_thread.joinable()) {
        /* Check if thread actually finished */
        if (impl_->thread_finished.load()) {
          /* Thread has exited cleanly, join it */
          impl_->worker_thread.join();
          break;
        }

        /* Check timeout */
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > timeout) {
          /* Thread didn't finish in time - this indicates a serious problem */
          fprintf(stderr, "ERROR: SSE worker thread did not finish within 30 seconds. Forcing join...\n");
          fflush(stderr);
          impl_->worker_thread.join();
          break;
        }

        /* Sleep briefly to avoid busy-wait */
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }

    impl_->connected = false;

    /* Additional delay to let any driver threads finish their cleanup.
     * This prevents race conditions with NVIDIA driver threads that may
     * still be accessing Windows handles during their TLS cleanup phase.
     * See: NVIDIA driver crash in tbbmalloc during thread shutdown. */
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

bool SseClient::is_connected() const
{
  return impl_ && impl_->connected;
}

/** \} */

}  // namespace blender::sse

#endif  /* _WIN32 */
