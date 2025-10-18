/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 *
 * Unix HTTP Client Implementation using POSIX sockets
 */

#ifndef WIN32

#  include "BLI_gem_http_client.hh"

#  include <arpa/inet.h>
#  include <netdb.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <cstring>
#  include <sstream>

namespace blender::http {

/* -------------------------------------------------------------------- */
/** \name Helper Functions
 * \{ */

/**
 * Parse URL into components (host, port, path).
 */
static bool parse_url(const std::string &url, std::string &host, int &port, std::string &path)
{
  /* Expected format: http://host:port/path or http://host/path */

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

  if (path_start != std::string::npos) {
    host_port = remainder.substr(0, path_start);
    path = remainder.substr(path_start);
  }
  else {
    host_port = remainder;
    path = "/";
  }

  /* Parse host and port */
  size_t port_start = host_port.find(':');

  if (port_start != std::string::npos) {
    host = host_port.substr(0, port_start);
    std::string port_str = host_port.substr(port_start + 1);
    port = std::stoi(port_str);
  }
  else {
    host = host_port;
    port = 80; /* Default HTTP port */
  }

  return !host.empty();
}

/**
 * Parse HTTP response to extract status code and body.
 */
static bool parse_http_response(const std::string &response, int &status_code, std::string &body)
{
  /* Find status line */
  size_t status_end = response.find("\r\n");
  if (status_end == std::string::npos) {
    return false;
  }

  std::string status_line = response.substr(0, status_end);

  /* Parse status code from status line (e.g., "HTTP/1.1 200 OK") */
  size_t code_start = status_line.find(' ');
  if (code_start == std::string::npos) {
    return false;
  }

  size_t code_end = status_line.find(' ', code_start + 1);
  if (code_end == std::string::npos) {
    code_end = status_line.length();
  }

  std::string code_str = status_line.substr(code_start + 1, code_end - code_start - 1);
  status_code = std::stoi(code_str);

  /* Find body (after \r\n\r\n) */
  size_t body_start = response.find("\r\n\r\n");
  if (body_start != std::string::npos) {
    body = response.substr(body_start + 4);
  }
  else {
    body = "";
  }

  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API Implementation
 * \{ */

HttpResponse http_get(const std::string &url, int timeout_ms)
{
  HttpResponse response;

  /* Parse URL */
  std::string host, path;
  int port;

  if (!parse_url(url, host, port, path)) {
    response.error_message = "Failed to parse URL";
    return response;
  }

  /* Resolve hostname */
  struct hostent *server = gethostbyname(host.c_str());
  if (server == nullptr) {
    response.error_message = "Failed to resolve hostname";
    return response;
  }

  /* Create socket */
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    response.error_message = "Failed to create socket";
    return response;
  }

  /* Set timeout */
  if (timeout_ms > 0) {
    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  }

  /* Setup server address */
  struct sockaddr_in serv_addr;
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
  serv_addr.sin_port = htons(port);

  /* Connect to server */
  if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    response.error_message = "Failed to connect to server";
    close(sockfd);
    return response;
  }

  /* Build HTTP request */
  std::ostringstream request;
  request << "GET " << path << " HTTP/1.1\r\n";
  request << "Host: " << host << "\r\n";
  request << "Connection: close\r\n";
  request << "\r\n";

  std::string request_str = request.str();

  /* Send request */
  if (send(sockfd, request_str.c_str(), request_str.length(), 0) < 0) {
    response.error_message = "Failed to send HTTP request";
    close(sockfd);
    return response;
  }

  /* Receive response */
  std::ostringstream response_stream;
  char buffer[4096];
  ssize_t bytes_received;

  while ((bytes_received = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
    buffer[bytes_received] = '\0';
    response_stream << buffer;
  }

  close(sockfd);

  std::string response_str = response_stream.str();

  /* Parse response */
  if (!parse_http_response(response_str, response.status_code, response.body)) {
    response.error_message = "Failed to parse HTTP response";
    return response;
  }

  response.success = true;
  return response;
}

HttpResponse http_post(const std::string &url, const std::string &body, int timeout_ms)
{
  HttpResponse response;

  /* Parse URL */
  std::string host, path;
  int port;

  if (!parse_url(url, host, port, path)) {
    response.error_message = "Failed to parse URL";
    return response;
  }

  /* Resolve hostname */
  struct hostent *server = gethostbyname(host.c_str());
  if (server == nullptr) {
    response.error_message = "Failed to resolve hostname";
    return response;
  }

  /* Create socket */
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    response.error_message = "Failed to create socket";
    return response;
  }

  /* Set timeout */
  if (timeout_ms > 0) {
    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  }

  /* Setup server address */
  struct sockaddr_in serv_addr;
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
  serv_addr.sin_port = htons(port);

  /* Connect to server */
  if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    response.error_message = "Failed to connect to server";
    close(sockfd);
    return response;
  }

  /* Build HTTP request */
  std::ostringstream request;
  request << "POST " << path << " HTTP/1.1\r\n";
  request << "Host: " << host << "\r\n";
  request << "Content-Type: application/json\r\n";
  request << "Content-Length: " << body.length() << "\r\n";
  request << "Connection: close\r\n";
  request << "\r\n";
  request << body;

  std::string request_str = request.str();

  /* Send request */
  if (send(sockfd, request_str.c_str(), request_str.length(), 0) < 0) {
    response.error_message = "Failed to send HTTP request";
    close(sockfd);
    return response;
  }

  /* Receive response */
  std::ostringstream response_stream;
  char buffer[4096];
  ssize_t bytes_received;

  while ((bytes_received = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
    buffer[bytes_received] = '\0';
    response_stream << buffer;
  }

  close(sockfd);

  std::string response_str = response_stream.str();

  /* Parse response */
  if (!parse_http_response(response_str, response.status_code, response.body)) {
    response.error_message = "Failed to parse HTTP response";
    return response;
  }

  response.success = true;
  return response;
}

/** \} */

}  // namespace blender::http

#endif /* !WIN32 */
