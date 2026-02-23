#include "../../inc/Helper/TiKVUdsClient.h"

#include <chrono>
#include <errno.h>
#include <future>
#include <iostream>
#include <memory>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace SPTAG
{
namespace Helper
{

static constexpr uint32_t kMagicReq = 0x53505447; // 'SPTG'
static constexpr uint32_t kMagicRes = 0x52535054; // 'RSPT'

static constexpr uint16_t kOpGet = 1;
static constexpr uint16_t kOpMultiGet = 2;
static constexpr uint16_t kOpPut = 3;
static constexpr uint16_t kOpDelete = 4;
static constexpr uint16_t kOpHealth = 5;

static inline void PutLE16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}
static inline void PutLE32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}
static inline void PutLE64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}
static inline uint32_t GetLE32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t GetLE64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= ((uint64_t)p[i] << (8 * i));
    return v;
}

TiKVUdsClient::TiKVUdsClient(const std::string &udsPath) : m_udsPath(udsPath)
{
}

TiKVUdsClient::~TiKVUdsClient()
{
    ShutDown();
}

bool TiKVUdsClient::Available() const
{
    return m_fd.load() >= 0;
}

void TiKVUdsClient::ShutDown()
{
    Disconnect();

    std::thread sendT, recvT;
    {
        std::lock_guard<std::mutex> g(m_connMu);
        sendT = std::move(m_sendThread);
        recvT = std::move(m_recvThread);
    }
    if (sendT.joinable())
        sendT.join();
    if (recvT.joinable())
        recvT.join();
}

void TiKVUdsClient::Disconnect()
{
    int expected = m_fd.load();
    if (expected >= 0)
    {
        std::lock_guard<std::mutex> g(m_connMu);
        if (m_fd.load() >= 0)
        {
            m_stopping = true;
            m_sendCv.notify_all();

            ::shutdown(m_fd.load(), SHUT_RDWR);
            ::close(m_fd.load());
            m_fd = -1;

            std::lock_guard<std::mutex> pg(m_pendingMu);
            for (auto &pair : m_pendingRequests)
            {
                ResponsePayload rp;
                rp.status = 3;
                pair.second->set_value(rp);
            }
            m_pendingRequests.clear();

            std::queue<QueuedRequest> empty;
            std::swap(m_sendQueue, empty);
        }
    }
}

bool TiKVUdsClient::EnsureConnected()
{
    if (m_fd.load() >= 0)
        return true;

    std::thread oldSend, oldRecv;
    {
        std::lock_guard<std::mutex> g(m_connMu);
        if (m_fd.load() >= 0)
            return true;

        oldSend = std::move(m_sendThread);
        oldRecv = std::move(m_recvThread);
    }

    if (oldSend.joinable())
        oldSend.join();
    if (oldRecv.joinable())
        oldRecv.join();

    std::lock_guard<std::mutex> g(m_connMu);
    if (m_fd.load() >= 0)
        return true;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (m_udsPath.size() >= sizeof(addr.sun_path))
    {
        ::close(fd);
        return false;
    }
    strncpy(addr.sun_path, m_udsPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        std::cerr << "TiKV UDS Connect Failed: " << strerror(errno) << " (errno " << errno << ")\n";
        ::close(fd);
        return false;
    }

    int snd = 4 << 20;
    int rcv = 4 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));

    m_fd = fd;
    m_stopping = false;
    m_sendThread = std::thread(&TiKVUdsClient::SendThreadMain, this);
    m_recvThread = std::thread(&TiKVUdsClient::RecvThreadMain, this);

    return true;
}

bool TiKVUdsClient::SendAllBlocking(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = ::send(fd, p + sent, len - sent, 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

bool TiKVUdsClient::RecvAllBlocking(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t recvd = 0;
    while (recvd < len)
    {
        ssize_t n = ::recv(fd, p + recvd, len - recvd, MSG_WAITALL);
        if (n == 0)
            return false;
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        recvd += (size_t)n;
    }
    return true;
}

void TiKVUdsClient::SendThreadMain()
{
    while (!m_stopping)
    {
        QueuedRequest req;
        {
            std::unique_lock<std::mutex> lock(m_sendMu);
            m_sendCv.wait(lock, [this] { return m_stopping || !m_sendQueue.empty(); });
            if (m_stopping)
                break;

            req = std::move(m_sendQueue.front());
            m_sendQueue.pop();
        }

        int fd = m_fd.load();
        if (fd < 0 || !SendAllBlocking(fd, req.data.data(), req.data.size()))
        {
            Disconnect();
            break;
        }
    }
}

void TiKVUdsClient::RecvThreadMain()
{
    while (!m_stopping)
    {
        int fd = m_fd.load();
        if (fd < 0)
            break;

        uint8_t hdr[16];
        if (!RecvAllBlocking(fd, hdr, sizeof(hdr)))
        {
            Disconnect();
            break;
        }

        if (GetLE32(hdr) != kMagicRes)
        {
            Disconnect();
            break;
        }

        uint64_t rid = GetLE64(hdr + 4);
        int32_t st = (int32_t)GetLE32(hdr + 12);

        uint8_t lenBuf[4];
        if (!RecvAllBlocking(fd, lenBuf, sizeof(lenBuf)))
        {
            Disconnect();
            break;
        }

        uint32_t plen = GetLE32(lenBuf);
        std::vector<uint8_t> payloadData(plen);
        if (plen > 0)
        {
            if (!RecvAllBlocking(fd, payloadData.data(), plen))
            {
                Disconnect();
                break;
            }
        }

        std::shared_ptr<std::promise<ResponsePayload>> p;
        {
            std::lock_guard<std::mutex> g(m_pendingMu);
            auto it = m_pendingRequests.find(rid);
            if (it != m_pendingRequests.end())
            {
                p = it->second;
                m_pendingRequests.erase(it);
            }
        }

        if (p)
        {
            ResponsePayload rp;
            rp.status = st;
            rp.data = std::move(payloadData);
            p->set_value(std::move(rp));
        }
    }
}

int32_t TiKVUdsClient::SendRequestAndWait(uint64_t rid, const std::vector<uint8_t> &reqData, ResponsePayload &out,
                                          const std::chrono::microseconds &timeout)
{
    if (!EnsureConnected())
        return 3;

    auto p = std::make_shared<std::promise<ResponsePayload>>();
    auto f = p->get_future();

    {
        std::lock_guard<std::mutex> g(m_pendingMu);
        m_pendingRequests[rid] = p;
    }

    {
        std::lock_guard<std::mutex> g(m_sendMu);
        m_sendQueue.push({reqData});
    }
    m_sendCv.notify_one();

    if (f.wait_for(timeout) == std::future_status::timeout)
    {
        std::lock_guard<std::mutex> g(m_pendingMu);
        m_pendingRequests.erase(rid);
        return 2;
    }

    out = f.get();
    return out.status;
}

int32_t TiKVUdsClient::Health(const std::chrono::microseconds &timeout)
{
    uint64_t rid = m_reqId++;
    std::vector<uint8_t> req(24, 0);
    PutLE32(req.data() + 0, kMagicReq);
    PutLE16(req.data() + 4, kOpHealth);
    PutLE64(req.data() + 8, rid);
    PutLE64(req.data() + 16, (uint64_t)timeout.count());

    ResponsePayload out;
    return SendRequestAndWait(rid, req, out, timeout);
}

int32_t TiKVUdsClient::GetU64(uint64_t key, std::string &value, const std::chrono::microseconds &timeout)
{
    uint64_t rid = m_reqId++;
    std::vector<uint8_t> req(32, 0);
    PutLE32(req.data() + 0, kMagicReq);
    PutLE16(req.data() + 4, kOpGet);
    PutLE64(req.data() + 8, rid);
    PutLE64(req.data() + 16, (uint64_t)timeout.count());
    PutLE64(req.data() + 24, key);

    ResponsePayload out;
    int32_t st = SendRequestAndWait(rid, req, out, timeout);
    if (st != 0)
        return st;

    if (out.data.size() < 4)
        return 3;
    uint32_t vlen = GetLE32(out.data.data());
    if (out.data.size() != 4 + vlen)
        return 3;
    value.assign((const char *)out.data.data() + 4, (size_t)vlen);
    return 0;
}

int32_t TiKVUdsClient::MultiGetU64(const std::vector<uint64_t> &keys, std::vector<std::string> &values,
                                   const std::chrono::microseconds &timeout)
{
    uint64_t rid = m_reqId++;
    uint32_t n = (uint32_t)keys.size();

    std::vector<uint8_t> req(24 + 4 + 8ULL * n, 0);
    PutLE32(req.data() + 0, kMagicReq);
    PutLE16(req.data() + 4, kOpMultiGet);
    PutLE64(req.data() + 8, rid);
    PutLE64(req.data() + 16, (uint64_t)timeout.count());

    PutLE32(req.data() + 24, n);
    for (uint32_t i = 0; i < n; i++)
        PutLE64(req.data() + 28 + 8ULL * i, keys[i]);

    ResponsePayload out;
    int32_t st = SendRequestAndWait(rid, req, out, timeout);
    if (st != 0)
        return st;

    if (out.data.size() < 4)
        return 3;
    uint32_t rn = GetLE32(out.data.data());
    if (rn != n)
        return 3;

    values.clear();
    values.resize(n);

    size_t off = 4;
    for (uint32_t i = 0; i < n; i++)
    {
        if (off >= out.data.size())
            return 3;
        uint8_t present = out.data[off++];
        if (!present)
        {
            values[i].clear();
            continue;
        }
        if (off + 4 > out.data.size())
            return 3;
        uint32_t vlen = GetLE32(out.data.data() + off);
        off += 4;
        if (off + vlen > out.data.size())
            return 3;
        values[i].assign((const char *)out.data.data() + off, (size_t)vlen);
        off += vlen;
    }
    return 0;
}

int32_t TiKVUdsClient::PutU64(uint64_t key, const std::string &value, const std::chrono::microseconds &timeout)
{
    uint64_t rid = m_reqId++;
    std::vector<uint8_t> req(24 + 8 + 4 + value.size(), 0);
    PutLE32(req.data() + 0, kMagicReq);
    PutLE16(req.data() + 4, kOpPut);
    PutLE64(req.data() + 8, rid);
    PutLE64(req.data() + 16, (uint64_t)timeout.count());

    PutLE64(req.data() + 24, key);
    PutLE32(req.data() + 32, (uint32_t)value.size());
    if (!value.empty())
    {
        memcpy(req.data() + 36, value.data(), value.size());
    }

    ResponsePayload out;
    return SendRequestAndWait(rid, req, out, timeout);
}

int32_t TiKVUdsClient::DeleteU64(uint64_t key, const std::chrono::microseconds &timeout)
{
    uint64_t rid = m_reqId++;
    std::vector<uint8_t> req(24 + 8, 0);
    PutLE32(req.data() + 0, kMagicReq);
    PutLE16(req.data() + 4, kOpDelete);
    PutLE64(req.data() + 8, rid);
    PutLE64(req.data() + 16, (uint64_t)timeout.count());
    PutLE64(req.data() + 24, key);

    ResponsePayload out;
    return SendRequestAndWait(rid, req, out, timeout);
}

} // namespace Helper
} // namespace SPTAG
