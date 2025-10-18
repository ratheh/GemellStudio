/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Windows HTTP Client Implementation using WinHTTP
 */

#ifdef WIN32

#  include "BLI_gem_http_client.hh"

#  include <sstream>

#  include "BLI_winstuff.h"
#  include <winhttp.h>

#  pragma comment(lib, "winhttp.lib")

namespace blender::http {

/* -------------------------------------------------------------------- */
/** \name Helper Functions
 * \{ */

/**
 * Parse URL into components (host, port, path).
 * Simple parser for http://host:port/path format.
 */
static bool parse_url(const std::string &url,
                      std::wstring &host,
                      int &port,
                      std::wstring &path)
{
  /* Expected format: http://host:port/path or http://host/path */
  std::string url_lower = url;

  /* Find protocol */
  size_t proto_end = url.find("://");
  if (proto_end == std::string::npos) {
    return false;
  }

  /* Extract host and path portion */
  std::string remainder = url.substr(proto_end + 3);

  /* Find path start */
  size_t path_start = remainder.find('/');
  std::string host_port;
  std::string path_str;

  if (path_start != std::string::npos) {
    host_port = remainder.substr(0, path_start);
    path_str = remainder.substr(path_start);
  }
  else {
    host_port = remainder;
    path_str = "/";
  }

  /* Parse host and port */
  size_t port_start = host_port.find(':');
  std::string host_str;

  if (port_start != std::string::npos) {
    host_str = host_port.substr(0, port_start);
    std::string port_str = host_port.substr(port_start + 1);
    port = std::stoi(port_str);
  }
  else {
    host_str = host_port;
    port = 80; /* Default HTTP port */
  }

  /* Convert to wide strings */
  int host_len = MultiByteToWideChar(CP_UTF8, 0, host_str.c_str(), -1, nullptr, 0);
  if (host_len > 0) {
    wchar_t *wide_host = new wchar_t[host_len];
    MultiByteToWideChar(CP_UTF8, 0, host_str.c_str(), -1, wide_host, host_len);
    host = wide_host;
    delete[] wide_host;
  }

  int path_len = MultiByteToWideChar(CP_UTF8, 0, path_str.c_str(), -1, nullptr, 0);
  if (path_len > 0) {
    wchar_t *wide_path = new wchar_t[path_len];
    MultiByteToWideChar(CP_UTF8, 0, path_str.c_str(), -1, wide_path, path_len);
    path = wide_path;
    delete[] wide_path;
  }

  return !host_str.empty();
}

/**
 * Read response body from WinHTTP request.
 */
static std::string read_response_body(HINTERNET h_request)
{
  std::ostringstream body;
  DWORD bytes_available = 0;
  DWORD bytes_read = 0;
  char buffer[4096];

  do {
    bytes_available = 0;
    if (!WinHttpQueryDataAvailable(h_request, &bytes_available)) {
      break;
    }

    if (bytes_available == 0) {
      break;
    }

    DWORD bytes_to_read = (bytes_available > sizeof(buffer)) ? sizeof(buffer) : bytes_available;
    if (!WinHttpReadData(h_request, buffer, bytes_to_read, &bytes_read)) {
      break;
    }

    if (bytes_read > 0) {
      body.write(buffer, bytes_read);
    }
  } while (bytes_available > 0);

  return body.str();
}

/**
 * Get HTTP status code from response.
 */
static int get_status_code(HINTERNET h_request)
{
  DWORD status_code = 0;
  DWORD size = sizeof(status_code);

  if (WinHttpQueryHeaders(h_request,
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX,
                          &status_code,
                          &size,
                          WINHTTP_NO_HEADER_INDEX))
  {
    return static_cast<int>(status_code);
  }

  return 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API Implementation
 * \{ */

HttpResponse http_get(const std::string &url, int timeout_ms)
{
  HttpResponse response;

  /* Parse URL */
  std::wstring host, path;
  int port;

  if (!parse_url(url, host, port, path)) {
    response.error_message = "Failed to parse URL";
    return response;
  }

  /* Initialize WinHTTP */
  HINTERNET h_session = WinHttpOpen(
      L"BlenderHTTPClient/1.0",
      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS,
      0);

  if (!h_session) {
    response.error_message = "Failed to initialize WinHTTP session";
    return response;
  }

  /* Set timeout */
  if (timeout_ms > 0) {
    WinHttpSetTimeouts(h_session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  }

  /* Connect to server */
  HINTERNET h_connect = WinHttpConnect(h_session, host.c_str(), port, 0);

  if (!h_connect) {
    response.error_message = "Failed to connect to server";
    WinHttpCloseHandle(h_session);
    return response;
  }

  /* Open request */
  HINTERNET h_request = WinHttpOpenRequest(h_connect,
                                           L"GET",
                                           path.c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           0);

  if (!h_request) {
    response.error_message = "Failed to open HTTP request";
    WinHttpCloseHandle(h_connect);
    WinHttpCloseHandle(h_session);
    return response;
  }

  /* Send request */
  if (!WinHttpSendRequest(
          h_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
  {
    response.error_message = "Failed to send HTTP request";
    WinHttpCloseHandle(h_request);
    WinHttpCloseHandle(h_connect);
    WinHttpCloseHandle(h_session);
    return response;
  }

  /* Receive response */
  if (!WinHttpReceiveResponse(h_request, nullptr)) {
    response.error_message = "Failed to receive HTTP response";
    WinHttpCloseHandle(h_request);
    WinHttpCloseHandle(h_connect);
    WinHttpCloseHandle(h_session);
    return response;
  }

  /* Get status code */
  response.status_code = get_status_code(h_request);

  /* Read response body */
  response.body = read_response_body(h_request);

  /* Success */
  response.success = true;

  /* Cleanup */
  WinHttpCloseHandle(h_request);
  WinHttpCloseHandle(h_connect);
  WinHttpCloseHandle(h_session);

  return response;
}

HttpResponse http_post(const std::string &url, const std::string &body, int timeout_ms)
{
  HttpResponse response;

  /* Parse URL */
  std::wstring host, path;
  int port;

  if (!parse_url(url, host, port, path)) {
    response.error_message = "Failed to parse URL";
    return response;
  }

  /* Initialize WinHTTP */
  HINTERNET h_session = WinHttpOpen(
      L"BlenderHTTPClient/1.0",
      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS,
      0);

  if (!h_session) {
    response.error_message = "Failed to initialize WinHTTP session";
    return response;
  }

  /* Set timeout */
  if (timeout_ms > 0) {
    WinHttpSetTimeouts(h_session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  }

  /* Connect to server */
  HINTERNET h_connect = WinHttpConnect(h_session, host.c_str(), port, 0);

  if (!h_connect) {
    response.error_message = "Failed to connect to server";
    WinHttpCloseHandle(h_session);
    return response;
  }

  /* Open request */
  HINTERNET h_request = WinHttpOpenRequest(h_connect,
                                           L"POST",
                                           path.c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           0);

  if (!h_request) {
    response.error_message = "Failed to open HTTP request";
    WinHttpCloseHandle(h_connect);
    WinHttpCloseHandle(h_session);
    return response;
  }

  /* Set Content-Type header */
  const wchar_t *headers = L"Content-Type: application/json\r\n";
  DWORD headers_len = wcslen(headers);

  /* Send request with body */
  if (!WinHttpSendRequest(h_request,
                          headers,
                          headers_len,
                          const_cast<char *>(body.c_str()),
                          static_cast<DWORD>(body.length()),
                          static_cast<DWORD>(body.length()),
                          0))
  {
    response.error_message = "Failed to send HTTP request";
    WinHttpCloseHandle(h_request);
    WinHttpCloseHandle(h_connect);
    WinHttpCloseHandle(h_session);
    return response;
  }

  /* Receive response */
  if (!WinHttpReceiveResponse(h_request, nullptr)) {
    response.error_message = "Failed to receive HTTP response";
    WinHttpCloseHandle(h_request);
    WinHttpCloseHandle(h_connect);
    WinHttpCloseHandle(h_session);
    return response;
  }

  /* Get status code */
  response.status_code = get_status_code(h_request);

  /* Read response body */
  response.body = read_response_body(h_request);

  /* Success */
  response.success = true;

  /* Cleanup */
  WinHttpCloseHandle(h_request);
  WinHttpCloseHandle(h_connect);
  WinHttpCloseHandle(h_session);

  return response;
}

/** \} */

}  // namespace blender::http

#endif /* WIN32 */
