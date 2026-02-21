#include "inc/Helper/TiKVUdsClient.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <string.h>

#include <stdexcept>

namespace SPTAG::Helper {

static constexpr uint32_t kMagicReq = 0x53505447; // 'SPTG'
static constexpr uint32_t kMagicRes = 0x52535054; // 'RSPT'

static constexpr uint16_t kOpGet      = 1;
static constexpr uint16_t kOpMultiGet = 2;
static constexpr uint16_t kOpPut      = 3;
static constexpr uint16_t kOpDelete   = 4;
static constexpr uint16_t kOpHealth   = 5;

static inline void PutLE16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}
static inline void PutLE32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}
static inline void PutLE64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8*i)) & 0xff);
}
static inline uint32_t GetLE32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t GetLE64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i] << (8*i));
    return v;
}

TiKVUdsClient::TiKVUdsClient(const std::string& udsPath)
    : m_udsPath(udsPath), m_fd(-1), m_reqId(1) {}

TiKVUdsClient::~TiKVUdsClient() { ShutDown(); }

bool TiKVUdsClient::Available() const { return m_fd >= 0; }

void TiKVUdsClient::ShutDown() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool TiKVUdsClient::EnsureConnected() {
    if (m_fd >= 0) return true;

    m_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_fd < 0) return false;

    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (m_udsPath.size() >= sizeof(addr.sun_path)) {
        ::close(m_fd);
        m_fd = -1;
        return false;
    }
    strncpy(addr.sun_path, m_udsPath.c_str(), sizeof(addr.sun_path)-1);

    if (::connect(m_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "TiKV UDS Connect Failed: " << strerror(errno) << " (errno " << errno << ")\n"; 
	::close(m_fd);
        m_fd = -1;
        return false;
    }

    int snd = 4 << 20;
    int rcv = 4 << 20;
    setsockopt(m_fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    setsockopt(m_fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    return true;
}

bool TiKVUdsClient::SendAll(const void* buf, size_t len, const std::chrono::microseconds& timeout) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;
    while (sent < len) {
        pollfd fds;
        fds.fd = m_fd;
        fds.events = POLLOUT;
        int rc = ::poll(&fds, 1, (int)std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
        if (rc <= 0) return false;

        ssize_t n = ::send(m_fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

bool TiKVUdsClient::RecvAll(void* buf, size_t len, const std::chrono::microseconds& timeout) {
    uint8_t* p = (uint8_t*)buf;
    size_t recvd = 0;
    while (recvd < len) {
        pollfd fds;
        fds.fd = m_fd;
        fds.events = POLLIN;
        int rc = ::poll(&fds, 1, (int)std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
        if (rc <= 0) return false;

        ssize_t n = ::recv(m_fd, p + recvd, len - recvd, 0);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        recvd += (size_t)n;
    }
    return true;
}

int32_t TiKVUdsClient::ReadResponse(uint64_t expectReqId, std::vector<uint8_t>& payload, const std::chrono::microseconds& timeout) {
    uint8_t hdr[16]; // magic(4) reqid(8) status(4)
    if (!RecvAll(hdr, sizeof(hdr), timeout)) return 3;
    if (GetLE32(hdr) != kMagicRes) return 3;
    uint64_t rid = GetLE64(hdr + 4);
    if (rid != expectReqId) return 3;
    int32_t st = (int32_t)GetLE32(hdr + 12);

    uint8_t lenBuf[4];
    if (!RecvAll(lenBuf, sizeof(lenBuf), timeout)) return 3;
    uint32_t plen = GetLE32(lenBuf);
    payload.resize(plen);
    if (plen > 0) {
        if (!RecvAll(payload.data(), plen, timeout)) return 3;
    }
    return st;
}

int32_t TiKVUdsClient::Health(const std::chrono::microseconds& timeout) {
    if (!EnsureConnected()) return 3;
    uint64_t rid = m_reqId++;

    uint8_t req[24];
    PutLE32(req + 0, kMagicReq);
    PutLE16(req + 4, kOpHealth);
    PutLE16(req + 6, 0);
    PutLE64(req + 8, rid);
    PutLE64(req + 16, (uint64_t)timeout.count());

    if (!SendAll(req, sizeof(req), timeout)) { ShutDown(); return 3; }
    std::vector<uint8_t> payload;
    int32_t st = ReadResponse(rid, payload, timeout);
    if (st == 3) ShutDown();
    return st;
}

int32_t TiKVUdsClient::GetU64(uint64_t key, std::string& value, const std::chrono::microseconds& timeout) {
    if (!EnsureConnected()) return 3;
    uint64_t rid = m_reqId++;

    uint8_t reqHdr[24];
    PutLE32(reqHdr + 0, kMagicReq);
    PutLE16(reqHdr + 4, kOpGet);
    PutLE16(reqHdr + 6, 0);
    PutLE64(reqHdr + 8, rid);
    PutLE64(reqHdr + 16, (uint64_t)timeout.count());

    uint8_t keyBuf[8];
    PutLE64(keyBuf, key);

    if (!SendAll(reqHdr, sizeof(reqHdr), timeout) || !SendAll(keyBuf, sizeof(keyBuf), timeout)) { ShutDown(); return 3; }

    std::vector<uint8_t> payload;
    int32_t st = ReadResponse(rid, payload, timeout);
    if (st != 0) {
        if (st == 3) ShutDown();
        return st;
    }
    if (payload.size() < 4) return 3;
    uint32_t vlen = GetLE32(payload.data());
    if (payload.size() != 4 + vlen) return 3;
    value.assign((const char*)payload.data() + 4, (size_t)vlen);
    return 0;
}

int32_t TiKVUdsClient::MultiGetU64(const std::vector<uint64_t>& keys, std::vector<std::string>& values, const std::chrono::microseconds& timeout) {
    if (!EnsureConnected()) return 3;
    uint64_t rid = m_reqId++;

    uint8_t reqHdr[24];
    PutLE32(reqHdr + 0, kMagicReq);
    PutLE16(reqHdr + 4, kOpMultiGet);
    PutLE16(reqHdr + 6, 0);
    PutLE64(reqHdr + 8, rid);
    PutLE64(reqHdr + 16, (uint64_t)timeout.count());

    uint32_t n = (uint32_t)keys.size();
    std::vector<uint8_t> body;
    body.resize(4 + 8ULL * n);
    PutLE32(body.data(), n);
    for (uint32_t i = 0; i < n; i++) PutLE64(body.data() + 4 + 8ULL*i, keys[i]);

    if (!SendAll(reqHdr, sizeof(reqHdr), timeout) || !SendAll(body.data(), body.size(), timeout)) { ShutDown(); return 3; }

    std::vector<uint8_t> payload;
    int32_t st = ReadResponse(rid, payload, timeout);
    if (st != 0) {
        if (st == 3) ShutDown();
        return st;
    }

    if (payload.size() < 4) return 3;
    uint32_t rn = GetLE32(payload.data());
    if (rn != n) return 3;

    values.clear();
    values.resize(n);

    size_t off = 4;
    for (uint32_t i = 0; i < n; i++) {
        if (off >= payload.size()) return 3;
        uint8_t present = payload[off++];
        if (!present) {
            values[i].clear();
            continue;
        }
        if (off + 4 > payload.size()) return 3;
        uint32_t vlen = GetLE32(payload.data() + off);
        off += 4;
        if (off + vlen > payload.size()) return 3;
        values[i].assign((const char*)payload.data() + off, (size_t)vlen);
        off += vlen;
    }
    return 0;
}

int32_t TiKVUdsClient::PutU64(uint64_t key, const std::string& value, const std::chrono::microseconds& timeout) {
    if (!EnsureConnected()) return 3;
    uint64_t rid = m_reqId++;

    uint8_t reqHdr[24];
    PutLE32(reqHdr + 0, kMagicReq);
    PutLE16(reqHdr + 4, kOpPut);
    PutLE16(reqHdr + 6, 0);
    PutLE64(reqHdr + 8, rid);
    PutLE64(reqHdr + 16, (uint64_t)timeout.count());

    uint8_t keyBuf[8];
    PutLE64(keyBuf, key);

    uint8_t vlenBuf[4];
    PutLE32(vlenBuf, (uint32_t)value.size());

    if (!SendAll(reqHdr, sizeof(reqHdr), timeout) ||
        !SendAll(keyBuf, sizeof(keyBuf), timeout) ||
        !SendAll(vlenBuf, sizeof(vlenBuf), timeout) ||
        (value.size() > 0 && !SendAll(value.data(), value.size(), timeout))) { ShutDown(); return 3; }

    std::vector<uint8_t> payload;
    int32_t st = ReadResponse(rid, payload, timeout);
    if (st == 3) ShutDown();
    return st;
}

int32_t TiKVUdsClient::DeleteU64(uint64_t key, const std::chrono::microseconds& timeout) {
    if (!EnsureConnected()) return 3;
    uint64_t rid = m_reqId++;

    uint8_t reqHdr[24];
    PutLE32(reqHdr + 0, kMagicReq);
    PutLE16(reqHdr + 4, kOpDelete);
    PutLE16(reqHdr + 6, 0);
    PutLE64(reqHdr + 8, rid);
    PutLE64(reqHdr + 16, (uint64_t)timeout.count());

    uint8_t keyBuf[8];
    PutLE64(keyBuf, key);

    if (!SendAll(reqHdr, sizeof(reqHdr), timeout) || !SendAll(keyBuf, sizeof(keyBuf), timeout)) { ShutDown(); return 3; }

    std::vector<uint8_t> payload;
    int32_t st = ReadResponse(rid, payload, timeout);
    if (st == 3) ShutDown();
    return st;
}

} // namespace SPTAG::Helper
