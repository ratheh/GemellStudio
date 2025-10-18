/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bli
 *
 * Platform-independent HTTP Client
 *
 * Provides a simple cross-platform HTTP client for making GET and POST requests.
 * Platform-specific implementations are in BLI_http_client_win.cc (Windows) and
 * BLI_http_client_unix.cc (Linux/macOS).
 *
 * This is intentionally a minimal implementation focused on REST API communication
 * with local services (localhost).
 */

#include <string>

namespace blender::http {

/* -------------------------------------------------------------------- */
/** \name HTTP Response
 * \{ */

/**
 * HTTP response structure containing status, body, and error information.
 */
struct HttpResponse {
  /** HTTP status code (e.g., 200 for OK, 404 for Not Found) */
  int status_code;

  /** Response body as a string */
  std::string body;

  /** Whether the request was successful */
  bool success;

  /** Error message if success is false */
  std::string error_message;

  HttpResponse() : status_code(0), success(false) {}
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name HTTP Client Functions
 * \{ */

/**
 * Perform an HTTP GET request.
 *
 * \param url: Full URL to request (e.g., "http://127.0.0.1:5000/health")
 * \param timeout_ms: Request timeout in milliseconds (0 = default timeout)
 * \return HTTP response structure
 *
 * \note This function blocks until the request completes or times out.
 */
HttpResponse http_get(const std::string &url, int timeout_ms = 5000);

/**
 * Perform an HTTP POST request with a JSON body.
 *
 * \param url: Full URL to request
 * \param body: Request body (typically JSON string)
 * \param timeout_ms: Request timeout in milliseconds (0 = default timeout)
 * \return HTTP response structure
 *
 * \note Content-Type is automatically set to "application/json"
 */
HttpResponse http_post(const std::string &url, const std::string &body, int timeout_ms = 5000);

/** \} */

}  // namespace blender::http
