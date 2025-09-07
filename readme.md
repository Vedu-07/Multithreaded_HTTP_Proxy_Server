# Multi-threaded HTTP/HTTPS Proxy Server (C++)

## Simple Server 

A lightweight multi-threaded proxy server written in C++ that supports both HTTP and HTTPS tunneling.
It is designed to demonstrate system-level programming concepts like socket programming, concurrency, synchronization, and caching.

## Features

### 1) Multi-threaded Client Handling

Uses std::thread to handle multiple client connections in parallel.

Semaphore-based control to limit max concurrent clients.

### 2) LRU Cache with TTL

Frequently requested pages are cached in memory.

Expired entries are auto-removed using Time-To-Live (TTL) logic.

Reduces repeated server lookups and improves performance.

### 3) HTTP Support

Handles standard HTTP methods like GET / POST.

Rebuilds requests, strips proxy headers, and forwards clean requests to origin servers.

### 4) HTTPS (CONNECT Method)

Supports HTTPS tunneling via the CONNECT method.

Establishes a bidirectional socket tunnel between client and target server.

Secure end-to-end — proxy doesn’t decrypt traffic.

### 5) Robust Error Handling

Graceful handling of bad requests, invalid hosts, or broken connections.

Ignores SIGPIPE to prevent crashes on failed sends.

### 6) Detailed Logging

Thread-safe logging with levels (INFO, DEBUG, ERROR).

Helps trace connections, data transfer, and cache activity.

### 7) Lightweight & Self-Contained

No external dependencies beyond the C++ standard library & POSIX sockets.

Runs directly from terminal.

## How It Works

1) Client (e.g., browser or curl) sends a request to proxy.

2) Proxy parses request → checks cache (for HTTP).

3) If cached → response is returned instantly.

4) If not cached → proxy forwards request to origin server.

5) Response from server is stored in cache and sent back to client.

6) For HTTPS → proxy establishes a tunnel and just forwards encrypted traffic.

## How to Run

### Getting Started
 1️) Build
``` bash
g++ -std=c++17 proxy.cpp -o proxy -lpthread 

or

make
```
2️) Run
``` bash
./proxy 8081
```

3️) Test with curl
```bash
curl -x http://localhost:8081 http://httpforever.com
```

3️) For HTTPS

``` bash
curl -x http://localhost:8081 https://example.com
```

## Disclaimer

This proxy is built for learning purposes only (networking, C++, caching, concurrency).
It is not production-ready and should not be used in sensitive environments.