// Full epoll-based HTTP proxy with LRU+TTL cache, Content-Length & chunked handling,
// CONNECT (HTTPS) tunneling, semaphore limiting concurrent clients, and improved logging.


#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;

// ---------------- Configuration ----------------
constexpr int DEFAULT_PORT = 8080;
constexpr int MAX_EVENTS = 2048;
constexpr int BUFFER_SIZE = 8192;
constexpr size_t CACHE_CAPACITY = 200;
constexpr int CACHE_TTL_SECONDS = 60;
constexpr int MAX_CONCURRENT_CLIENTS = 100;

// ---------------- Logger ----------------
class Logger {
public:
    enum class Level { INFO, DEBUG, ERROR };

    void log(Level level, const std::string &msg) {
        std::lock_guard<std::mutex> lock(mu_);
        const char *lvl = (level == Level::INFO) ? "[INFO] " : (level == Level::DEBUG) ? "[DEBUG] " : "[ERROR]";
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char timebuf[64];
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(timebuf, sizeof(timebuf), "%F %T", &tmv);
        std::cout << timebuf << " " << lvl << " " << msg << std::endl;
    }

    void info(const std::string &m) { log(Level::INFO, m); }
    void debug(const std::string &m) { log(Level::DEBUG, m); }
    void error(const std::string &m) { log(Level::ERROR, m); }

private:
    std::mutex mu_;
};

static Logger logger;

// ---------------- Semaphore + active client count ----------------
static sem_t clientSemaphore;
static std::atomic<int> activeClients{0};

// ---------------- LRU Cache with TTL ----------------
struct CacheNode {
    std::string key;
    std::string value; // raw http response (headers + body)
    std::chrono::steady_clock::time_point expiry;
};

class LRUCache {
public:
    LRUCache(size_t cap, int ttl) : capacity_(cap), ttlSeconds_(ttl) {}

    bool find(const std::string &key, std::string &value) {
        logger.debug("[CACHE] Attempting to acquire cache mutex for find()");
        mtx_.lock();
        logger.debug("[CACHE] Mutex ACQUIRED for find(): " + key);

        auto it = map_.find(key);
        if (it == map_.end()) {
            logger.debug("[CACHE] Cache MISS for key: " + key);
            logger.debug("[CACHE] Mutex RELEASED for find()");
            mtx_.unlock();
            return false;
        }

        auto &node = *(it->second);
        auto now = std::chrono::steady_clock::now();
        if (now > node.expiry) {
            logger.debug("[CACHE] Cache entry EXPIRED for key: " + key);
            items_.erase(it->second);
            map_.erase(it);
            logger.debug("[CACHE] Mutex RELEASED for find()");
            mtx_.unlock();
            return false;
        }

        // move to front
        items_.splice(items_.begin(), items_, it->second);
        value = node.value;
        logger.debug("[CACHE] Cache HIT for key: " + key);

        logger.debug("[CACHE] Mutex RELEASED for find()");
        mtx_.unlock();
        return true;
    }

    void put(const std::string &key, const std::string &value) {
        logger.debug("[CACHE] Attempting to acquire cache mutex for put()");
        mtx_.lock();
        logger.debug("[CACHE] Mutex ACQUIRED for put(): " + key);

        auto it = map_.find(key);
        if (it != map_.end()) {
            items_.erase(it->second);
            map_.erase(it);
            logger.debug("[CACHE] Updated existing cache entry: " + key);
        }

        items_.push_front(CacheNode{key, value, std::chrono::steady_clock::now() + std::chrono::seconds(ttlSeconds_)});
        map_[key] = items_.begin();

        if (items_.size() > capacity_) {
            auto last = items_.end(); --last;
            logger.debug("[CACHE] Evicting LRU entry: " + last->key);
            map_.erase(last->key);
            items_.pop_back();
        }

        logger.debug("[CACHE] Inserted key into cache: " + key);
        logger.debug("[CACHE] Mutex RELEASED for put()");
        mtx_.unlock();
    }

private:
    size_t capacity_;
    int ttlSeconds_;
    std::list<CacheNode> items_;
    std::unordered_map<std::string, std::list<CacheNode>::iterator> map_;
    std::mutex mtx_;
};

static LRUCache cache(CACHE_CAPACITY, CACHE_TTL_SECONDS);

// ---------------- Utilities ----------------
static int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static std::string toLower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }

bool parseRequestLineSimple(const std::string &line, std::string &method, std::string &url, std::string &version) {
    std::istringstream iss(line);
    if (iss >> method >> url >> version) return true;
    return false;
}

// Find end of headers (returns position after \r\n\r\n or npos)
size_t findHeaderEnd(const std::string &buf) {
    const std::string needle = "\r\n\r\n";
    size_t pos = buf.find(needle);
    if (pos == std::string::npos) return std::string::npos;
    return pos + needle.size();
}

// Extract Host header
bool extractHostFromHeaders(const std::string &req, std::string &host, int &port) {
    std::istringstream iss(req);
    std::string line;
    if (!std::getline(iss, line)) return false; // skip request line
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        std::string low = line; for (char &c : low) c = (char)tolower((unsigned char)c);
        if (low.rfind("host:", 0) == 0) {
            std::string value = line.substr(5);
            // trim
            size_t a = 0; while (a < value.size() && isspace((unsigned char)value[a])) ++a;
            size_t b = value.size(); while (b > a && isspace((unsigned char)value[b-1])) --b;
            value = value.substr(a, b - a);
            size_t colon = value.find(':');
            if (colon != std::string::npos) {
                host = value.substr(0, colon);
                port = std::stoi(value.substr(colon + 1));
            } else {
                host = value;
                port = 80;
            }
            return true;
        }
    }
    return false;
}

// Build request line for origin: convert absolute URI to path if needed and preserve headers (Host kept)
std::string buildOriginRequest(const std::string &rawReq) {
    std::istringstream iss(rawReq);
    std::string firstLine;
    if (!std::getline(iss, firstLine)) return "";
    if (!firstLine.empty() && firstLine.back() == '\r') firstLine.pop_back();

    std::string method, url, version;
    if (!parseRequestLineSimple(firstLine, method, url, version)) return "";

    std::string path = url;
    if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0) {
        // strip scheme and host
        size_t p = path.find("://");
        if (p != std::string::npos) {
            size_t start = p + 3;
            size_t slash = path.find('/', start);
            if (slash != std::string::npos) path = path.substr(slash);
            else path = "/";
        }
    }
    if (path.empty()) path = "/";

    std::ostringstream out;
    out << method << " " << path << " " << version << "\r\n";

    // copy headers, skip Proxy-Connection/Proxy-Authorization, keep Host as-is
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        std::string low = line; for (char &c : low) c = (char)tolower((unsigned char)c);
        if (low.rfind("proxy-connection:", 0) == 0) continue;
        if (low.rfind("proxy-authorization:", 0) == 0) continue;
        out << line << "\r\n";
    }
    // ensure Connection: close to simplify responses
    out << "Connection: close\r\n\r\n";
    return out.str();
}

// Non-blocking connect helper (returns fd >=0 or -1). Caller must set epoll to watch EPOLLOUT/EPOLLIN as appropriate
int nonblockingConnect(const std::string &host, int port) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
    if (rc != 0 || !res) {
        logger.error("[DNS] getaddrinfo failed for " + host + ":" + std::to_string(port) + " -> " + gai_strerror(rc));
        if (res) freeaddrinfo(res);
        return -1;
    }

    int sd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sd < 0) {
        freeaddrinfo(res);
        logger.error("[CONNECT] socket() failed");
        return -1;
    }

    setNonBlocking(sd);
    rc = connect(sd, res->ai_addr, res->ai_addrlen);
    if (rc == 0) {
        freeaddrinfo(res);
        return sd; // connected immediately
    }
    if (rc < 0 && errno != EINPROGRESS) {
        logger.error("[CONNECT] connect() failed: " + std::string(strerror(errno)));
        close(sd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    return sd; // EINPROGRESS
}

// ---------------- Connection state machine ----------------
enum class ConnState {
    READING_CLIENT,
    PROCESSING,
    CONNECTING_ORIGIN,
    WRITING_ORIGIN,
    READING_ORIGIN,
    WRITING_CLIENT,
    TUNNEL, // CONNECT tunnel established and relaying
    CLOSED
};

struct Connection {
    int clientFd{-1};
    int serverFd{-1};
    ConnState state{ConnState::READING_CLIENT};
    std::string clientBuf;   // bytes read from client (request)
    std::string originReq;   // request string to send to origin
    std::string originBuf;   // bytes read from origin (headers + body) for caching & forwarding
    std::string clientOut;   // bytes to send to client (from cache or originBuf)
    std::string serverOut;   // bytes to send to origin (originReq or remainder)
    std::string urlKey;      // canonical URL used as cache key
    std::string host;
    int port{80};
    std::chrono::steady_clock::time_point lastActive;

    Connection(int cfd=-1): clientFd(cfd) { lastActive = std::chrono::steady_clock::now(); }
};

static std::unordered_map<int, std::shared_ptr<Connection>> fdToConn;

// Cleanup helper
void cleanupConnection(int epfd, std::shared_ptr<Connection> conn) {
    if (!conn) return;
    if (conn->clientFd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, conn->clientFd, nullptr);
        close(conn->clientFd);
        fdToConn.erase(conn->clientFd);
        logger.debug("[CLEANUP] Closed client fd=" + std::to_string(conn->clientFd));
        conn->clientFd = -1;
    }
    if (conn->serverFd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, conn->serverFd, nullptr);
        close(conn->serverFd);
        fdToConn.erase(conn->serverFd);
        logger.debug("[CLEANUP] Closed server fd=" + std::to_string(conn->serverFd));
        conn->serverFd = -1;
    }
    conn->state = ConnState::CLOSED;
}

// Build canonical URL key and host from clientBuf
bool buildUrlKeyAndHost(std::shared_ptr<Connection> conn) {
    std::string method, url, version;
    // parse first line
    std::istringstream iss(conn->clientBuf);
    std::string firstLine;
    if (!std::getline(iss, firstLine)) return false;
    if (!firstLine.empty() && firstLine.back() == '\r') firstLine.pop_back();
    if (!parseRequestLineSimple(firstLine, method, url, version)) return false;

    if (method == "CONNECT") {
        // CONNECT host:port -> tunnel
        size_t colon = url.find(':');
        if (colon != std::string::npos) {
            conn->host = url.substr(0, colon);
            conn->port = stoi(url.substr(colon+1));
        } else {
            conn->host = url;
            conn->port = 443;
        }
        conn->urlKey = ""; // no cache key for CONNECT
        return true;
    }

    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
        // absolute URI; get host and path
        size_t p = url.find("://");
        size_t start = (p==std::string::npos) ? 0 : p+3;
        size_t slash = url.find('/', start);
        std::string hostport;
        std::string path = "/";
        if (slash == std::string::npos) {
            hostport = url.substr(start);
            path = "/";
        } else {
            hostport = url.substr(start, slash - start);
            path = url.substr(slash);
        }
        size_t colon = hostport.find(':');
        if (colon != std::string::npos) {
            conn->host = hostport.substr(0, colon);
            conn->port = stoi(hostport.substr(colon+1));
        } else {
            conn->host = hostport;
            conn->port = 80;
        }
        conn->urlKey = std::string("http://") + conn->host + path;
    } else {
        // relative path; find Host header
        int port = 80; std::string host;
        if (!extractHostFromHeaders(conn->clientBuf, host, port)) return false;
        conn->host = host; conn->port = port;
        // get path from request line
        std::istringstream iss2(conn->clientBuf);
        std::string line;
        std::getline(iss2, line);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream p2(line);
        std::string method_, path_, ver_;
        p2 >> method_ >> path_ >> ver_;
        conn->urlKey = std::string("http://") + host + path_;
    }
    return true;
}

// detect if full origin response is available (by Content-Length or chunked terminator)
bool isOriginResponseComplete(const std::string &buf) {
    size_t hdrEnd = findHeaderEnd(buf);
    if (hdrEnd == std::string::npos) return false;
    std::string headers = buf.substr(0, hdrEnd);
    std::string lower = toLower(headers);

    if (lower.find("transfer-encoding: chunked") != std::string::npos) {
        if (buf.find("\r\n0\r\n\r\n", hdrEnd) != std::string::npos) return true;
        return false;
    }
    size_t pos = lower.find("content-length:");
    if (pos != std::string::npos) {
        pos += strlen("content-length:");
        while (pos < lower.size() && isspace((unsigned char)lower[pos])) pos++;
        size_t end = pos; while (end < lower.size() && isdigit((unsigned char)lower[end])) end++;
        std::string num = lower.substr(pos, end-pos);
        try {
            long long clen = std::stoll(num);
            size_t bodyLen = buf.size() - hdrEnd;
            return (long long)bodyLen >= clen;
        } catch (...) { return false; }
    }
    return false; // otherwise rely on server close
}

// ---------------- CONNECT tunnel helper (used when method == CONNECT) ----------------
// After we've nonblocking connected to origin, we send "200 Connection Established" to client
// then switch both fds to epoll relay (TUNNEL) and simply forward bytes
void startTunnel(int epfd, std::shared_ptr<Connection> conn) {
    // Send 200 to client
    const char *ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
    send(conn->clientFd, ok, strlen(ok), 0);
    conn->state = ConnState::TUNNEL;
    logger.info("[CONNECT] Tunnel established for " + conn->host + ":" + std::to_string(conn->port) +
                ". Note: HTTPS cannot be cached because traffic is encrypted end-to-end.");
    // ensure both fds are in epoll (they are). We will handle EPOLLIN events on both fds and forward.
}

// ---------------- main epoll loop ----------------
int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);

    signal(SIGPIPE, SIG_IGN);

    sem_init(&clientSemaphore, 0, MAX_CONCURRENT_CLIENTS);
    logger.info("Semaphore initialized with max concurrent clients = " + std::to_string(MAX_CONCURRENT_CLIENTS));

    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) { logger.error("socket() failed"); return 1; }
    int opt = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (setNonBlocking(listenFd) < 0) { logger.error("setNonBlocking(listenFd) failed"); return 1; }

    sockaddr_in sv{};
    sv.sin_family = AF_INET; sv.sin_addr.s_addr = INADDR_ANY; sv.sin_port = htons(port);
    if (bind(listenFd, (sockaddr*)&sv, sizeof(sv)) < 0) { logger.error("bind() failed"); return 1; }
    if (listen(listenFd, SOMAXCONN) < 0) { logger.error("listen() failed"); return 1; }

    int epfd = epoll_create1(0);
    if (epfd < 0) { logger.error("epoll_create1 failed"); return 1; }
    epoll_event lev{}; lev.events = EPOLLIN; lev.data.fd = listenFd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenFd, &lev) < 0) { logger.error("epoll_ctl ADD listenFd failed"); return 1; }

    logger.info("Epoll proxy listening on port " + std::to_string(port));

    std::vector<epoll_event> events(MAX_EVENTS);

    while (true) {
        int n = epoll_wait(epfd, events.data(), (int)events.size(), -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            logger.error("epoll_wait error"); break;
        }

        for (int i=0;i<n;++i) {
            int fd = events[i].data.fd;
            uint32_t evts = events[i].events;

            // New incoming
            if (fd == listenFd) {
                while (true) {
                    sockaddr_in cli{}; socklen_t cliLen = sizeof(cli);
                    int clientFd = accept(listenFd, (sockaddr*)&cli, &cliLen);
                    if (clientFd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        logger.error("accept() failed: " + std::string(strerror(errno)));
                        break;
                    }

                    logger.debug("[SEMAPHORE] Attempting to acquire semaphore for client fd=" + std::to_string(clientFd));
                    sem_wait(&clientSemaphore);
                    int curr = ++activeClients;
                    logger.info("[SEMAPHORE] Acquired by fd=" + std::to_string(clientFd) + " | Active clients: " + std::to_string(curr));

                    setNonBlocking(clientFd);
                    epoll_event cev{}; cev.events = EPOLLIN | EPOLLET; cev.data.fd = clientFd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, clientFd, &cev) < 0) {
                        logger.error("epoll_ctl ADD clientFd failed");
                        close(clientFd);
                        sem_post(&clientSemaphore);
                        --activeClients;
                        continue;
                    }

                    auto conn = std::make_shared<Connection>(clientFd);
                    fdToConn[clientFd] = conn;

                    char ipbuf[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &cli.sin_addr, ipbuf, sizeof(ipbuf));
                    logger.info("Accepted client " + std::string(ipbuf) + " fd=" + std::to_string(clientFd));
                }
                continue;
            }

            // Existing connection event
            auto it = fdToConn.find(fd);
            if (it == fdToConn.end()) {
                logger.debug("Event for unknown fd=" + std::to_string(fd));
                continue;
            }
            auto conn = it->second;

            // error/hup
            if (evts & (EPOLLERR | EPOLLHUP)) {
                logger.info("EPOLLERR/HUP on fd=" + std::to_string(fd));
                // If client fd closed, release semaphore
                cleanupConnection(epfd, conn);
                sem_post(&clientSemaphore);
                int curr = --activeClients;
                logger.info("[SEMAPHORE] Released (error) | Active clients: " + std::to_string(curr));
                continue;
            }

            // --- If event on client fd ---
            if (fd == conn->clientFd) {
                // if in tunnel mode, just read from client and forward to server
                if (conn->state == ConnState::TUNNEL) {
                    if (evts & EPOLLIN) {
                        // forward all available bytes to serverFd
                        bool closed = false;
                        while (true) {
                            char buf[BUFFER_SIZE];
                            ssize_t r = read(conn->clientFd, buf, sizeof(buf));
                            if (r > 0) {
                                if (conn->serverFd >= 0) send(conn->serverFd, buf, r, 0);
                            } else if (r == 0) { closed = true; break; }
                            else { if (errno==EAGAIN||errno==EWOULDBLOCK) break; closed=true; break; }
                        }
                        if (closed) {
                            cleanupConnection(epfd, conn);
                            sem_post(&clientSemaphore);
                            int curr = --activeClients;
                            logger.info("[SEMAPHORE] Released (tunnel client close) | Active clients: " + std::to_string(curr));
                        }
                    }
                    continue;
                }

                // normal HTTP flow
                if (evts & EPOLLIN) {
                    bool clientClosed = false;
                    while (true) {
                        char buf[BUFFER_SIZE];
                        ssize_t r = read(conn->clientFd, buf, sizeof(buf));
                        if (r > 0) conn->clientBuf.append(buf, buf + r);
                        else if (r == 0) { clientClosed = true; break; }
                        else { if (errno==EAGAIN||errno==EWOULDBLOCK) break; clientClosed=true; break; }
                    }
                    if (clientClosed) {
                        logger.info("Client closed fd=" + std::to_string(conn->clientFd));
                        cleanupConnection(epfd, conn);
                        sem_post(&clientSemaphore);
                        int curr = --activeClients;
                        logger.info("[SEMAPHORE] Released (client close) | Active clients: " + std::to_string(curr));
                        continue;
                    }

                    size_t hdrEnd = findHeaderEnd(conn->clientBuf);
                    if (hdrEnd != std::string::npos && conn->state == ConnState::READING_CLIENT) {
                        conn->state = ConnState::PROCESSING;
                        logger.debug("Received full request headers from client fd=" + std::to_string(conn->clientFd));

                        if (!buildUrlKeyAndHost(conn)) {
                            logger.error("Failed to build url/host; closing conn");
                            cleanupConnection(epfd, conn);
                            sem_post(&clientSemaphore);
                            int curr = --activeClients;
                            logger.info("[SEMAPHORE] Released (parse fail) | Active clients: " + std::to_string(curr));
                            continue;
                        }

                        // If method was CONNECT, handle specially
                        { // detect method
                            std::istringstream iss(conn->clientBuf);
                            std::string reqLine;
                            std::getline(iss, reqLine);
                            if (!reqLine.empty() && reqLine.back()=='\r') reqLine.pop_back();
                            std::string method, url, version;
                            parseRequestLineSimple(reqLine, method, url, version);
                            if (method == "CONNECT") {
                                // CONNECT: make non-blocking connect to host:port, then send 200 and start tunnel
                                logger.info("CONNECT request for " + conn->host + ":" + std::to_string(conn->port) + " from fd=" + std::to_string(conn->clientFd));
                                // Log that HTTPS cannot be cached
                                logger.info("[CACHE] HTTPS/CONNECT requests will NOT be cached for " + conn->host + ":" + std::to_string(conn->port));

                                int sfd = nonblockingConnect(conn->host, conn->port);
                                if (sfd < 0) {
                                    const char *resp = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
                                    send(conn->clientFd, resp, strlen(resp), 0);
                                    cleanupConnection(epfd, conn);
                                    sem_post(&clientSemaphore);
                                    int curr = --activeClients;
                                    logger.info("[SEMAPHORE] Released (connect fail) | Active clients: " + std::to_string(curr));
                                    continue;
                                }
                                conn->serverFd = sfd;
                                fdToConn[sfd] = conn;
                                // mark both fds (server and client) for EPOLLIN (we'll forward)
                                epoll_event sev{}; sev.events = EPOLLIN | EPOLLET; sev.data.fd = sfd;
                                epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &sev);
                                // Wait until connect completes: when EPOLLOUT observed on server fd, we will send 200 and switch to TUNNEL.
                                conn->state = ConnState::CONNECTING_ORIGIN;
                                logger.info("[CONNECT] Initiated non-blocking connect to " + conn->host + ":" + std::to_string(conn->port) + " sfd=" + std::to_string(sfd));
                                continue;
                            }
                        }

                        // For non-CONNECT methods: check cache
                        logger.info("Request URL key: " + conn->urlKey + " host=" + conn->host + " port=" + std::to_string(conn->port));
                        std::string cached;
                        if (!conn->urlKey.empty() && cache.find(conn->urlKey, cached)) {
                            logger.info("[CACHE] HIT for " + conn->urlKey + " -> serving cached response");
                            conn->clientOut = cached;
                            conn->state = ConnState::WRITING_CLIENT;
                            epoll_event mod{}; mod.events = EPOLLIN | EPOLLOUT | EPOLLET; mod.data.fd = conn->clientFd;
                            epoll_ctl(epfd, EPOLL_CTL_MOD, conn->clientFd, &mod);
                            continue;
                        }

                        // cache miss -> build origin request
                        conn->originReq = buildOriginRequest(conn->clientBuf);
                        conn->serverOut = conn->originReq;
                        logger.debug("[PROXY] Cache miss: will connect to origin " + conn->host);

                        int sfd = nonblockingConnect(conn->host, conn->port);
                        if (sfd < 0) {
                            logger.error("[CONNECT] Could not connect to origin: " + conn->host);
                            cleanupConnection(epfd, conn);
                            sem_post(&clientSemaphore);
                            int curr = --activeClients;
                            logger.info("[SEMAPHORE] Released (connect fail) | Active clients: " + std::to_string(curr));
                            continue;
                        }
                        conn->serverFd = sfd;
                        fdToConn[sfd] = conn;
                        epoll_event sev{}; sev.events = EPOLLOUT | EPOLLIN; sev.data.fd = sfd;
                        epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &sev);
                        conn->state = ConnState::CONNECTING_ORIGIN;
                        logger.info("[CONNECT] Initiated non-blocking connect to " + conn->host + ":" + std::to_string(conn->port) + " sfd=" + std::to_string(sfd));
                    }
                } // EPOLLIN on client

                // EPOLLOUT on client: send cached/origin bytes to client
                if (evts & EPOLLOUT) {
                    if (!conn->clientOut.empty()) {
                        while (!conn->clientOut.empty()) {
                            ssize_t w = write(conn->clientFd, conn->clientOut.data(), conn->clientOut.size());
                            if (w > 0) conn->clientOut.erase(0, (size_t)w);
                            else { if (errno==EAGAIN||errno==EWOULDBLOCK) break; logger.error("[WRITE] client write error"); cleanupConnection(epfd, conn); break; }
                        }
                        if (conn->clientOut.empty()) {
                            logger.info("[DONE] Finished writing to client fd=" + std::to_string(conn->clientFd) + ". Closing connection.");
                            cleanupConnection(epfd, conn);
                            sem_post(&clientSemaphore);
                            int curr = --activeClients;
                            logger.info("[SEMAPHORE] Released (done writing) | Active clients: " + std::to_string(curr));
                        }
                    }
                }
            } // client fd branch

            // --- If event on server fd (origin) ---
            if (conn->serverFd >= 0 && fd == conn->serverFd) {
                // If we were connecting (for either CONNECT tunnel or normal request)
                if (conn->state == ConnState::CONNECTING_ORIGIN && (evts & EPOLLOUT)) {
                    int err = 0; socklen_t len = sizeof(err);
                    if (getsockopt(conn->serverFd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
                        logger.error("[CONNECT] getsockopt failed");
                        cleanupConnection(epfd, conn);
                        sem_post(&clientSemaphore);
                        int curr = --activeClients;
                        logger.info("[SEMAPHORE] Released (getsockopt fail) | Active clients: " + std::to_string(curr));
                        continue;
                    }
                    if (err != 0) {
                        logger.error("[CONNECT] connect error: " + std::string(strerror(err)));
                        cleanupConnection(epfd, conn);
                        sem_post(&clientSemaphore);
                        int curr = --activeClients;
                        logger.info("[SEMAPHORE] Released (connect err) | Active clients: " + std::to_string(curr));
                        continue;
                    }

                    // If original request was CONNECT, now establish tunnel
                    // Detect by checking the original client request first line
                    std::istringstream iss(conn->clientBuf);
                    std::string reqLine; std::getline(iss, reqLine);
                    if (!reqLine.empty() && reqLine.back()=='\r') reqLine.pop_back();
                    std::string method, url, version;
                    parseRequestLineSimple(reqLine, method, url, version);
                    if (method == "CONNECT") {
                        // From here, both fds are ready for relaying
                        startTunnel(epfd, conn);
                        // ensure server fd is set to non-blocking and monitored for EPOLLIN
                        epoll_event modS{}; modS.events = EPOLLIN | EPOLLET; modS.data.fd = conn->serverFd;
                        epoll_ctl(epfd, EPOLL_CTL_MOD, conn->serverFd, &modS);
                        // similarly ensure client fd has EPOLLIN for receiving tunnel data
                        epoll_event modC{}; modC.events = EPOLLIN | EPOLLET; modC.data.fd = conn->clientFd;
                        epoll_ctl(epfd, EPOLL_CTL_MOD, conn->clientFd, &modC);
                        continue;
                    }

                    // else it's normal HTTP: proceed to write originReq
                    conn->state = ConnState::WRITING_ORIGIN;
                    logger.info("[CONNECT] Connected to origin sfd=" + std::to_string(conn->serverFd) + ", sending request");
                }

                // EPOLLOUT: continue writing serverOut
                if (evts & EPOLLOUT) {
                    if (!conn->serverOut.empty()) {
                        while (!conn->serverOut.empty()) {
                            ssize_t w = write(conn->serverFd, conn->serverOut.data(), conn->serverOut.size());
                            if (w > 0) conn->serverOut.erase(0, (size_t)w);
                            else { if (errno==EAGAIN||errno==EWOULDBLOCK) break; logger.error("[WRITE] server write error"); cleanupConnection(epfd, conn); break; }
                        }
                        if (conn->serverOut.empty()) {
                            conn->state = ConnState::READING_ORIGIN;
                            epoll_event mod{}; mod.events = EPOLLIN | EPOLLET; mod.data.fd = conn->serverFd;
                            epoll_ctl(epfd, EPOLL_CTL_MOD, conn->serverFd, &mod);
                            logger.debug("[FORWARD] Request forwarded to origin for url=" + conn->urlKey);
                        }
                    }
                }

                // EPOLLIN: read origin data
                if (evts & EPOLLIN) {
                    bool originClosed = false;
                    while (true) {
                        char buf[BUFFER_SIZE];
                        ssize_t r = read(conn->serverFd, buf, sizeof(buf));
                        if (r > 0) conn->originBuf.append(buf, buf + r);
                        else if (r == 0) { originClosed = true; break; }
                        else { if (errno==EAGAIN||errno==EWOULDBLOCK) break; originClosed = true; break; }
                    }

                    if (isOriginResponseComplete(conn->originBuf)) {
                        logger.info("[ORIGIN] Full response received for url=" + conn->urlKey + " size=" + std::to_string(conn->originBuf.size()) + " bytes");
                        // Cache it (only for HTTP responses, not CONNECT)
                        if (!conn->urlKey.empty()) {
                            cache.put(conn->urlKey, conn->originBuf);
                            logger.info("[CACHE] Stored response for " + conn->urlKey);
                        }
                        // queue to client
                        conn->clientOut = conn->originBuf;
                        conn->originBuf.clear();
                        conn->state = ConnState::WRITING_CLIENT;
                        epoll_event modc{}; modc.events = EPOLLIN | EPOLLOUT | EPOLLET; modc.data.fd = conn->clientFd;
                        epoll_ctl(epfd, EPOLL_CTL_MOD, conn->clientFd, &modc);

                        // close server fd (we're done with origin)
                        epoll_ctl(epfd, EPOLL_CTL_DEL, conn->serverFd, nullptr);
                        close(conn->serverFd);
                        fdToConn.erase(conn->serverFd);
                        conn->serverFd = -1;
                    } else if (originClosed) {
                        // origin closed and we have partial response -> accept it
                        if (!conn->originBuf.empty()) {
                            logger.info("[ORIGIN] Origin closed, using collected bytes for url=" + conn->urlKey);
                            if (!conn->urlKey.empty()) {
                                cache.put(conn->urlKey, conn->originBuf);
                                logger.info("[CACHE] Stored response for " + conn->urlKey);
                            }
                            conn->clientOut = conn->originBuf;
                            conn->originBuf.clear();
                            conn->state = ConnState::WRITING_CLIENT;
                            epoll_event modc{}; modc.events = EPOLLIN | EPOLLOUT | EPOLLET; modc.data.fd = conn->clientFd;
                            epoll_ctl(epfd, EPOLL_CTL_MOD, conn->clientFd, &modc);
                        } else {
                            cleanupConnection(epfd, conn);
                            sem_post(&clientSemaphore);
                            int curr = --activeClients;
                            logger.info("[SEMAPHORE] Released (origin closed empty) | Active clients: " + std::to_string(curr));
                        }
                    } else {
                        // not complete yet, wait for more EPOLLIN events
                    }
                } // EPOLLIN server
            } // server fd branch
        } // for events
    } // while epoll_wait

    close(listenFd);
    sem_destroy(&clientSemaphore);
    return 0;
}
