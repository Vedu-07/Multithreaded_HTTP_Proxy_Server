// Header Files
// You included a lot of headers:
// Networking:
// <arpa/inet.h>, <netdb.h>, <unistd.h> → needed for sockets (send/recv, IP handling).
// Multithreading & sync:
// <thread>, <mutex>, <semaphore.h>, <atomic> → handle multiple clients at the same time safely.
// Utilities:
// <chrono> (time), <sstream> (string processing), <vector>, <unordered_map>, <list> (for cache).
// I/O:
// <iostream> for printing logs.

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <list>
#include <mutex>
#include <netdb.h>
#include <semaphore.h>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <unistd.h>

#define PORT 8081 // proxy listens on port 8081.
#define BUFFER_SIZE 8192 // max data chunk size (8 KB).
#define CACHE_TTL 60 // cache validity = 60 seconds.
#define MAX_CONCURRENT_CLIENTS 10 // only allow 10 clients at a time.

// ---------------- Globals ----------------
// Semaphore → controls how many clients can connect at the same time (like a ticket system).
// Atomic counter → keeps track of number of currently connected clients in a thread-safe way.
sem_t clientSemaphore;
std::atomic<int> activeClients{0};

// ---------------- Logger ----------------
// Logger Class
// This is a helper for printing logs from multiple threads without mixing them up:
// It has levels: Info, Debug, Error.
// log() locks a mutex so only one thread prints at a time.
// Convenience functions → info(), debug(), error().
// ✅ Example: If 2 clients print logs at the same time, without the mutex the output could get jumbled. This prevents that.

class Logger
{
public:
    enum class Level
    {
        Info,
        Debug,
        Error
    };

    void log(Level level, const std::string &msg)
    {
        // We still protect output with a mutex so logs from threads don't interleave.
        std::lock_guard<std::mutex> lock(mu);
        const char *lvl =
            (level == Level::Info ? "[INFO] " : (level == Level::Debug ? "[DEBUG] " : "[ERROR] "));
        std::cout << lvl << msg << std::endl;
    }

    // convenience wrappers
    void info(const std::string &m) { log(Level::Info, m); }
    void debug(const std::string &m) { log(Level::Debug, m); }
    void error(const std::string &m) { log(Level::Error, m); }

private:
    std::mutex mu;
};

Logger logger;

// ---------------- LRU Cache ----------------
// This is for caching web responses so repeated requests are faster:
// Node = small structure storing:
// key → usually the URL.
// value → the response body.
// expiry → when this cache entry becomes invalid (after TTL).
// capacity → how many items cache can hold.
// items → doubly-linked list (std::list) to maintain order (LRU = least recently used).
// map → fast lookup table from key → pointer to node in the list.
// mtx → protects cache from race conditions when multiple threads use it.
// ttl → how long data stays valid.


// proxy server will often fetch the same webpage multiple times.
// Instead of going to the internet every time, it stores the response in memory.
// Next time the same page is requested → serve directly from cache (faster).
// It uses LRU (Least Recently Used) policy → if cache is full, remove the oldest unused entry.

// 1. Node
// Each cache entry stores:
// key → usually the URL of the request.
// value → the page content (response from server).
// expiry → a time limit (so stale data doesn’t get reused).

// 2. Data Structures
// items (list) → Keeps nodes in recently used order.
// Most recently used → at the front.
// Least recently used → at the back.
// map (unordered_map) → Fast lookup: URL → iterator to.
// mtx (mutex) → Protects cache operations since multiple threads (clients) might read/write at the same time.
class LRUCache
{
    struct Node
    {
        std::string key;
        std::string value;
        std::chrono::steady_clock::time_point expiry;
    };

    size_t capacity;
    std::list<Node> items;
    std::unordered_map<std::string, decltype(items.begin())> map;
    std::mutex mtx;
    int ttl;

public:
    LRUCache(size_t cap, int ttlSeconds) : capacity(cap), ttl(ttlSeconds) {}

    // bool find(const std::string &key, std::string &value)
    // {
    //     // Log and acquire lock manually to show acquire/release in terminal
    //     logger.debug("[CACHE] Attempting to acquire cache mutex for find()");
    //     mtx.lock();
    //     logger.debug("[CACHE] Mutex ACQUIRED for find(): " + key);

    //     auto it = map.find(key);
    //     if (it == map.end())
    //     {
    //         logger.debug("[CACHE] Cache miss for key: " + key);
    //         logger.debug("[CACHE] Mutex RELEASED for find()");
    //         mtx.unlock();
    //         return false;
    //     }

    //     auto &node = *(it->second);
    //     if (std::chrono::steady_clock::now() > node.expiry)
    //     {
    //         logger.debug("[CACHE] Cache entry expired for key: " + key);
    //         items.erase(it->second);
    //         map.erase(it);
    //         logger.debug("[CACHE] Mutex RELEASED for find()");
    //         mtx.unlock();
    //         return false;
    //     }

    //     // move to front
    //     items.splice(items.begin(), items, it->second);
    //     value = node.value;
    //     logger.debug("[CACHE] Cache hit for key: " + key);

    //     logger.debug("[CACHE] MuteX RELEASED for find()");
    //     mtx.unlock();
    //     return true;
    // }



// find(key, value)
// Tries to find the page in cache.
// If not found → MISS (returns false).
// If found but expired → delete it, MISS.
// If found & valid → HIT (copy value, move entry to front because it’s most recently used).
//  This makes sure fresh items stay in cache longer.
bool find(const std::string &key, std::string &value)
{
    // Log and acquire lock manually to show acquire/release in terminal
    logger.debug("[CACHE] Attempting to acquire cache mutex for find()");
    mtx.lock();
    logger.debug("[CACHE] Mutex ACQUIRED for find(): " + key);

    auto it = map.find(key);
    if (it == map.end())
    {
        logger.debug("[CACHE] Cache MISS for key: " + key);
        logger.debug("[CACHE] Mutex RELEASED for find()");
        mtx.unlock();
        return false;
    }

    auto &node = *(it->second);
    auto now = std::chrono::steady_clock::now();

    if (now > node.expiry)
    {
        logger.debug("[CACHE] Cache entry EXPIRED for key: " + key);
        items.erase(it->second);
        map.erase(it);
        logger.debug("[CACHE] Mutex RELEASED for find()");
        mtx.unlock();
        return false;
    }

    // ✅ Cache hit: reuse stored value
    value = node.value;

    // Move accessed entry to the front (LRU policy)
    items.splice(items.begin(), items, it->second);

    logger.debug("[CACHE] Cache HIT for key: " + key + " (valid & reused)");
    logger.debug("[CACHE] Mutex RELEASED for find()");
    mtx.unlock();
    return true;
}

// -------- Put (Insert/Update cache entry) --------
// put(key, value)
// Adds/updates an entry in cache.
// If entry already exists → update it + refresh expiry + move to front.
// If cache is full → evict the least recently used entry (items.back()).
// Insert the new entry at the front.
// This ensures the cache doesn’t grow too large.
bool put(const std::string &key, const std::string &value)
{
    logger.debug("[CACHE] Attempting to acquire cache mutex for put()");
    mtx.lock();
    logger.debug("[CACHE] Mutex ACQUIRED for put(): " + key);

    auto it = map.find(key);
    auto now = std::chrono::steady_clock::now();

    if (it != map.end())
    {
        // Key already exists → update value and expiry, move to front
        logger.debug("[CACHE] Updating existing cache entry: " + key);
        auto &node = *(it->second);
        node.value = value;
        node.expiry = now + std::chrono::seconds(CACHE_TTL);

        items.splice(items.begin(), items, it->second); // move to front
        logger.debug("[CACHE] Entry refreshed and moved to front: " + key);

        logger.debug("[CACHE] Mutex RELEASED for put()");
        mtx.unlock();
        return true;
    }

    // If capacity is full → evict LRU
    if (items.size() >= capacity)
    {
        auto last = items.back();
        logger.debug("[CACHE] Cache FULL, evicting LRU entry: " + last.key);

        map.erase(last.key);
        items.pop_back();
    }

    // Insert new entry at front
    items.push_front(Node{key, value, now + std::chrono::seconds(CACHE_TTL)});
    map[key] = items.begin();
    logger.debug("[CACHE] New entry added to cache: " + key);

    logger.debug("[CACHE] Mutex RELEASED for put()");
    mtx.unlock();
    return true;
}

// create(key, value)
// Similar to put, but explicitly:
// Deletes old entry if exists.
// Evicts last if cache full.
// Inserts new one at front with new expiry.
// Difference: create always overwrites if key exists, while put updates if found.

void create(const std::string &key, const std::string &value)
    {
        logger.debug("[CACHE] Attempting to acquire cache mutex for create()");
        mtx.lock();
        logger.debug("[CACHE] Mutex ACQUIRED for create(): " + key);

        auto it = map.find(key);
        if (it != map.end())
        {
            logger.debug("[CACHE] Key already exists, updating cache entry for " + key);
            items.erase(it->second);
            map.erase(it);
        }

        if (items.size() == capacity)
        {
            auto last = items.back();
            map.erase(last.key);
            items.pop_back();
            logger.debug("[CACHE] Cache is full, evicting LRU item: " + last.key);
        }

        items.push_front({key, value,
                          std::chrono::steady_clock::now() + std::chrono::seconds(ttl)});
        map[key] = items.begin();

        logger.debug("[CACHE] Added/updated cache entry for: " + key);
        logger.debug("[CACHE] Mutex RELEASED for create()");
        mtx.unlock();
    }


// erase(key)
// Removes a key manually if it exists.
// Useful if server says the cached page is invalid (e.g., forced refresh).

void erase(const std::string &key)
    {
        logger.debug("[CACHE] Attempting to acquire cache mutex for erase()");
        mtx.lock();
        logger.debug("[CACHE] Mutex ACQUIRED for erase(): " + key);

        auto it = map.find(key);
        if (it != map.end())
        {
            items.erase(it->second);
            map.erase(it);
            logger.debug("[CACHE] Manually erased cache entry for key: " + key);
        }

        logger.debug("[CACHE] Mutex RELEASED for erase()");
        mtx.unlock();
    }
};

LRUCache cache(100, CACHE_TTL);

// ---------------- Utilities ----------------
// receiveData(int sock)
// This function reads data from a socket until there’s no more data left.
// Think of a socket as a pipe between your proxy and the client/server.
// Steps:
// char buffer[BUFFER_SIZE];
// → Temporary storage (8 KB at a time).
// std::ostringstream response;
// → We’ll keep appending data here to form one complete string.
// recv(sock, buffer, BUFFER_SIZE, 0)
// Reads bytes from the socket into buffer.
// Returns how many bytes were read (bytesRead).
// Returns 0 if the other side closed the connection.
// while ((bytesRead = recv(...)) > 0)
// Keep reading until there’s nothing left.
// Each chunk is written to response.
// Logging shows how many bytes were received each time.
// When done → return the whole message as a std::string.
// ✅ In practice:
// If reading from browser → this will capture the full HTTP request.
// If reading from server → this will capture the full HTTP response.
std::string receiveData(int sock)
{
    char buffer[BUFFER_SIZE];
    std::ostringstream response;
    ssize_t bytesRead;

    logger.debug("Receiving data from socket...");
    while ((bytesRead = recv(sock, buffer, BUFFER_SIZE, 0)) > 0)
    {
        response.write(buffer, bytesRead);
        logger.debug("Received " + std::to_string(bytesRead) + " bytes from peer socket.");
        // continue until remote closes or no more data
    }
    logger.debug("Finished receiving data from socket.");
    return response.str();
}
parseRequestLine(const std::string &requestLine, …)
// This is a simple HTTP parser for the first line of a request.
// Example HTTP request:
// GET http://example.com/index.html HTTP/1.1
// Steps:
// std::istringstream iss(requestLine);
// → Split the string into words.
// iss >> method >> url >> version
// method = "GET"
// url = "http://example.com/index.html"
// version = "HTTP/1.1"
// If parsing works → return true. Otherwise → false.
bool parseRequestLine(const std::string &requestLine, std::string &method, std::string &url, std::string &version)
{
    std::istringstream iss(requestLine);
    if (iss >> method >> url >> version)
        return true;
    return false;
}

// ---------------- Tunnel helpers ----------------
// It creates a data pipe between two sockets.
// fromSock → the socket you’re reading from (e.g., client → proxy).
// toSock → the socket you’re writing to (e.g., proxy → destination server).
// direction → just for logging ("client→server" or "server→client").
// Whats its used for?
// Normal HTTP proxying:
// When your proxy fetches data from a server and needs to stream it back to the client.
// HTTPS tunneling (CONNECT method):
// Browser says:
// CONNECT www.google.com:443 HTTP/1.1
// → Proxy must act like a bridge, just forwarding raw encrypted data both ways.
// tunnelAndLog is exactly what enables that.
// 👉 Essentially, this function keeps reading from one side and forwarding everything to the other side until the connection closes.

void tunnelAndLog(int fromSock, int toSock, const std::string &direction)
{
    char buf[BUFFER_SIZE];
    ssize_t n;
    size_t total = 0;
    while ((n = recv(fromSock, buf, sizeof(buf), 0)) > 0)
    {
        ssize_t s = send(toSock, buf, n, 0);
        if (s < 0)
            break;
        total += (size_t)s;
    }
    logger.info("Tunnel closed (" + direction + "), bytes transferred: " + std::to_string(total));
    // we won't close sockets here; main logic will close ends appropriately
}

// ---------------- Client Handler ----------------
// This function is called whenever a new client (like a browser) connects.
// Manages concurrency with semaphores.
// Parses request (method, URL, version).
// If CONNECT (HTTPS) → create a tunnel.
// If HTTP (GET/POST/etc.) → check cache → else forward request → cache response → send back.
// Logs everything for debugging.

void handleClient(int clientSock)
{
    // Get client IP for logs
    char client_ip[INET_ADDRSTRLEN] = "unknown";
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getpeername(clientSock, (struct sockaddr *)&addr, &len) == 0)
        inet_ntop(AF_INET, &(addr.sin_addr), client_ip, INET_ADDRSTRLEN);

    // Acquire semaphore limiting concurrent clients
    logger.debug("[SEMAPHORE] Attempting to acquire semaphore for client: " + std::string(client_ip));
    sem_wait(&clientSemaphore);
    int current = ++activeClients;
    logger.info("[SEMAPHORE] Acquired by " + std::string(client_ip) + " | Active clients: " + std::to_string(current));

    logger.info("New client connected from " + std::string(client_ip));

    // Read initial request (headers)
    char buffer[BUFFER_SIZE];
    ssize_t bytesRead = recv(clientSock, buffer, BUFFER_SIZE - 1, 0);
    if (bytesRead <= 0)
    {
        logger.error("Client disconnected or failed to read data from " + std::string(client_ip));
        close(clientSock);
        --activeClients;
        sem_post(&clientSemaphore);
        logger.debug("[SEMAPHORE] Released (read failed) | Active clients: " + std::to_string(activeClients.load()));
        return;
    }
    buffer[bytesRead] = '\0';
    std::string request(buffer);

    // parse request line
    std::istringstream reqStream(request);
    std::string reqLine;
    std::getline(reqStream, reqLine);
    if (!reqLine.empty() && reqLine.back() == '\r')
        reqLine.pop_back();

    std::string method, url, version;
    if (!parseRequestLine(reqLine, method, url, version))
    {
        logger.error("Failed to parse request line from client " + std::string(client_ip));
        close(clientSock);
        --activeClients;
        sem_post(&clientSemaphore);
        logger.debug("[SEMAPHORE] Released (parse failed) | Active clients: " + std::to_string(activeClients.load()));
        return;
    }
    logger.info("Request from " + std::string(client_ip) + ": " + method + " " + url);

    // ---------- Handle HTTPS CONNECT ----------
    //     Extract host + port (www.google.com, 443).
    // Resolve hostname (gethostbyname).
    // Connect to target server on port 443.
    // Reply back to client:
    // HTTP/1.1 200 Connection Established
    // Start two threads with tunnelAndLog:
    // Client → Server
    // Server → Client
    // After this, proxy just blindly forwards encrypted data.
    // No caching possible, since HTTPS is encrypted end-to-end.
    if (method == "CONNECT")
    {
        logger.info("HTTPS CONNECT request for " + url + " from " + std::string(client_ip));

        // extract host:port
        std::string host = url;
        int port = 443;
        size_t colonPos = host.find(':');
        if (colonPos != std::string::npos)
        {
            port = std::stoi(host.substr(colonPos + 1));
            host = host.substr(0, colonPos);
        }

        struct hostent *server = gethostbyname(host.c_str());
        if (!server)
        {
            logger.error("No such host (HTTPS): " + host);
            close(clientSock);
            --activeClients;
            sem_post(&clientSemaphore);
            logger.debug("[SEMAPHORE] Released (no host) | Active clients: " + std::to_string(activeClients.load()));
            return;
        }

        int serverSock = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSock < 0)
        {
            logger.error("Failed to create socket for HTTPS destination.");
            close(clientSock);
            --activeClients;
            sem_post(&clientSemaphore);
            logger.debug("[SEMAPHORE] Released (socket fail) | Active clients: " + std::to_string(activeClients.load()));
            return;
        }

        struct sockaddr_in servAddr{};
        servAddr.sin_family = AF_INET;
        servAddr.sin_port = htons(port);
        std::memcpy(&servAddr.sin_addr.s_addr, server->h_addr, server->h_length);

        if (connect(serverSock, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
        {
            logger.error("Connection to HTTPS server " + host + " failed.");
            close(serverSock);
            close(clientSock);
            --activeClients;
            sem_post(&clientSemaphore);
            logger.debug("[SEMAPHORE] Released (connect fail) | Active clients: " + std::to_string(activeClients.load()));
            return;
        }

        // Tell the browser the tunnel is ready
        std::string okResponse = "HTTP/1.1 200 Connection Established\r\n\r\n";
        send(clientSock, okResponse.c_str(), okResponse.size(), 0);
        logger.info("Tunnel established for " + host + ". Note: HTTPS cannot be cached because traffic is encrypted end-to-end.");

        // Start bidirectional tunnel with logging
        std::thread t1(tunnelAndLog, clientSock, serverSock, "client->server");
        std::thread t2(tunnelAndLog, serverSock, clientSock, "server->client");
        t1.detach();
        t2.detach();

        // We won't close sockets here — the tunnel threads will finish when either side closes.
        --activeClients;
        sem_post(&clientSemaphore);
        logger.debug("[SEMAPHORE] Released after CONNECT setup | Active clients: " + std::to_string(activeClients.load()));
        return;
    }

    // ---------- Handle HTTP (GET/etc) ----------
    // Try cache (URL used as key)
    //     For normal HTTP requests:

    // (a) Check Cache
    // if (cache.find(url, cachedResponse)) {
    //     send(clientSock, cachedResponse...);
    // }

    // If cached → send response back immediately.
    // Saves time & bandwidth.
    // (b) If Not Cached → Forward Request
    // Parse Host + Path
    // Extract host from URL or from Host: header.
    // Example:
    // URL: http://example.com/index.html
    // Host: example.com
    // Path: /index.html
    // Connect to Origin Server (port 80).
    // Rebuild Request
    // Forward client’s request to server.
    // Remove proxy-specific headers (Proxy-Connection, Proxy-Authorization).
    // Add Connection: close.
    // (c) Get Response from Server
    // std::string response = receiveData(serverSock);
    // Reads entire HTTP response (headers + body).
    //     std::string cachedResponse;

    if (cache.find(url, cachedResponse))
    {
        logger.info("Cache hit for " + url + ". Serving response from cache.");
        send(clientSock, cachedResponse.c_str(), cachedResponse.size(), 0);
        close(clientSock);
        --activeClients;
        sem_post(&clientSemaphore);
        logger.debug("[SEMAPHORE] Released after cache hit | Active clients: " + std::to_string(activeClients.load()));
        return;
    }
    logger.info("Cache miss for " + url + ". Forwarding request to origin server.");

    // Determine host and path
    std::string host = url;
    std::string path = "/";
    if (url.find("http://") == 0)
    {
        host = url.substr(7);
        size_t slash = host.find('/');
        if (slash != std::string::npos)
        {
            path = host.substr(slash);
            host = host.substr(0, slash);
        }
    }
    else
    {
        // If URL is absolute-path (typical when browser configured as proxy, first line contains full URL).
        // Try to extract Host header instead.
        std::string headerLine;
        std::istringstream tmpStream(request);
        std::getline(tmpStream, headerLine); // skip request line
        while (std::getline(tmpStream, headerLine) && headerLine != "\r")
        {
            if (headerLine.find("Host:") == 0 || headerLine.find("host:") == 0)
            {
                std::string hostHeader = headerLine.substr(headerLine.find(":") + 1);
                // trim spaces
                while (!hostHeader.empty() && (hostHeader.front() == ' ' || hostHeader.front() == '\t'))
                    hostHeader.erase(hostHeader.begin());
                if (!hostHeader.empty() && hostHeader.back() == '\r')
                    hostHeader.pop_back();
                size_t colon = hostHeader.find(':');
                if (colon != std::string::npos)
                    host = hostHeader.substr(0, colon);
                else
                    host = hostHeader;
                break;
            }
        }
    }

    logger.debug("Resolved request to host: " + host + " and path: " + path);

    struct hostent *server = gethostbyname(host.c_str());
    if (!server)
    {
        logger.error("No such host: " + host);
        close(clientSock);
        --activeClients;
        sem_post(&clientSemaphore);
        logger.debug("[SEMAPHORE] Released (no host) | Active clients: " + std::to_string(activeClients.load()));
        return;
    }

    int serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock < 0)
    {
        logger.error("Failed to create socket for destination server.");
        close(clientSock);
        --activeClients;
        sem_post(&clientSemaphore);
        logger.debug("[SEMAPHORE] Released (socket fail) | Active clients: " + std::to_string(activeClients.load()));
        return;
    }

    struct sockaddr_in servAddr{};
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(80);
    std::memcpy(&servAddr.sin_addr.s_addr, server->h_addr, server->h_length);

    logger.debug("Connecting to " + host + " on port 80.");
    if (connect(serverSock, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
    {
        logger.error("Connection to server " + host + " failed.");
        close(serverSock);
        close(clientSock);
        --activeClients;
        sem_post(&clientSemaphore);
        logger.debug("[SEMAPHORE] Released (connect fail) | Active clients: " + std::to_string(activeClients.load()));
        return;
    }
    logger.info("Successfully connected to " + host + ".");

    // Rebuild request to send to origin server — preserve headers mostly, drop Proxy-Connection
    std::ostringstream newReq;
    newReq << method << " " << path << " " << version << "\r\n";
    std::string headerLine;
    std::istringstream headerStream(request);
    std::getline(headerStream, headerLine); // skip request line we've already handled

    while (std::getline(headerStream, headerLine) && headerLine != "\r")
    {
        if (headerLine.find("Proxy-Connection:") == 0 || headerLine.find("Proxy-Authorization:") == 0)
            continue;
        // ensure we don't accidentally send an extra '\r' at line ends
        if (!headerLine.empty() && headerLine.back() == '\r')
            headerLine.pop_back();
        newReq << headerLine << "\r\n";
    }
    newReq << "Connection: close\r\n\r\n";

    std::string newReqStr = newReq.str();
    logger.debug("Forwarding request to server:\n" + newReqStr);
    send(serverSock, newReqStr.c_str(), newReqStr.size(), 0);
    logger.debug("Request sent. Waiting for response from origin server...");

    // Receive full response (headers + body)
    std::string response = receiveData(serverSock);
    logger.debug("Response received from server. Size: " + std::to_string(response.size()) + " bytes.");

    // Store in cache
    cache.create(url, response);
    logger.info("Stored response for " + url + " in cache.");

    // Send back to client
    ssize_t sent = send(clientSock, response.c_str(), response.size(), 0);
    if (sent < 0)
        logger.error("Failed to send response to client " + std::string(client_ip));
    else
        logger.info("Response sent back to client " + std::string(client_ip) + ". Closing connections.");

    close(serverSock);
    close(clientSock);

    --activeClients;
    sem_post(&clientSemaphore);
    logger.debug("[SEMAPHORE] Released after HTTP handling | Active clients: " + std::to_string(activeClients.load()));
}

// ---------------- Main ----------------
int main(int argc, char *argv[])
{
    int port = PORT;
    if (argc > 1)
    {
        try
        {
            port = std::stoi(argv[1]);
        }
        catch (...)
        {
            std::cerr << "Invalid port provided; using default " << PORT << std::endl;
            port = PORT;
        }
    }

    // Ignore SIGPIPE so failed sends don't kill the process
    signal(SIGPIPE, SIG_IGN);

    // Initialize semaphore
    sem_init(&clientSemaphore, 0, MAX_CONCURRENT_CLIENTS);
    logger.info("Semaphore initialized with max concurrent clients = " + std::to_string(MAX_CONCURRENT_CLIENTS));

    int serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock < 0)
    {
        logger.error("Failed to create server socket.");
        return 1;
    }

    // reuse address
    int opt = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(serverSock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
    {
        logger.error("Failed to bind to port " + std::to_string(port));
        close(serverSock);
        return 1;
    }
    logger.info("Socket bound successfully.");

    if (listen(serverSock, 50) < 0)
    {
        logger.error("Listen failed.");
        close(serverSock);
        return 1;
    }
    logger.info("Proxy server listening on port " + std::to_string(port));

    while (true)
    {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientSock = accept(serverSock, (struct sockaddr *)&clientAddr, &clientLen);
        if (clientSock < 0)
        {
            logger.error("Accept failed.");
            continue;
        }

        // spawn thread to handle client
        std::thread t(handleClient, clientSock);
        t.detach();
    }

    close(serverSock);
    sem_destroy(&clientSemaphore);
    return 0;
}
