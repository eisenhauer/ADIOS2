/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * XrootdHttpRemote.cpp - HTTP/HTTPS-based client for XRootD SSI services
 *
 * Uses a shared CurlMultiPool singleton for efficient parallel requests
 * with connection pooling across all XrootdHttpRemote instances.
 */

#include "XrootdHttpRemote.h"

#include <cctype>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>

#ifdef ADIOS2_HAVE_CURL
#include <curl/curl.h>
#endif

namespace adios2
{

#ifdef ADIOS2_HAVE_CURL

namespace
{

size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t totalSize = size * nmemb;
    std::vector<char> *buffer = static_cast<std::vector<char> *>(userp);
    char *data = static_cast<char *>(contents);
    buffer->insert(buffer->end(), data, data + totalSize);
    return totalSize;
}

} // anonymous namespace

// ======================================================================
// CurlMultiPool implementation
// ======================================================================

CurlMultiPool &CurlMultiPool::getInstance()
{
    static CurlMultiPool instance;
    return instance;
}

CurlMultiPool::CurlMultiPool()
{
    // curl_global_init() must be called before any other curl function.
    // It is not thread-safe, but C++11 guarantees that static local
    // initialization (in getInstance()) is serialized, so this constructor
    // runs exactly once with no concurrent curl calls possible.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    m_MultiHandle = curl_multi_init();
    if (!m_MultiHandle)
    {
        std::cerr << "CurlMultiPool: Failed to initialize CURL multi handle" << std::endl;
        return;
    }

    curl_multi_setopt(m_MultiHandle, CURLMOPT_MAXCONNECTS, 50L);
    curl_multi_setopt(m_MultiHandle, CURLMOPT_MAX_TOTAL_CONNECTIONS, 50L);
    curl_multi_setopt(m_MultiHandle, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);

    m_Running = true;
    m_WorkerThread = std::thread(&CurlMultiPool::WorkerLoop, this);
}

CurlMultiPool::~CurlMultiPool()
{
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        m_Running = false;
    }
    m_QueueCV.notify_all();

    if (m_WorkerThread.joinable())
    {
        m_WorkerThread.join();
    }

    if (m_MultiHandle)
    {
        curl_multi_cleanup(m_MultiHandle);
        m_MultiHandle = nullptr;
    }
}

void CurlMultiPool::Submit(CURL *easyHandle, AsyncGet *asyncOp)
{
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        m_PendingQueue.push_back({easyHandle, asyncOp});
    }
    m_QueueCV.notify_one();
}

void CurlMultiPool::ProcessCompletedTransfers()
{
    CURLMsg *msg;
    int msgsLeft;

    while ((msg = curl_multi_info_read(m_MultiHandle, &msgsLeft)))
    {
        if (msg->msg == CURLMSG_DONE)
        {
            CURL *easy = msg->easy_handle;
            CURLcode result = msg->data.result;

            AsyncGet *asyncOp = nullptr;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &asyncOp);

            if (asyncOp)
            {
                bool success = false;

                if (result == CURLE_OK)
                {
                    long httpCode = 0;
                    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &httpCode);

                    if (httpCode == 200 || httpCode == 201)
                    {
                        if (asyncOp->destBuffer && !asyncOp->responseData.empty())
                        {
                            memcpy(asyncOp->destBuffer, asyncOp->responseData.data(),
                                   asyncOp->responseData.size());
                            asyncOp->destSize = asyncOp->responseData.size();
                        }
                        success = true;
                    }
                    else
                    {
                        std::ostringstream ss;
                        ss << "HTTP error " << httpCode;
                        if (!asyncOp->responseData.empty())
                        {
                            ss << ": "
                               << std::string(asyncOp->responseData.begin(),
                                              asyncOp->responseData.end());
                        }
                        asyncOp->errorMsg = ss.str();
                    }
                }
                else
                {
                    asyncOp->errorMsg = curl_easy_strerror(result);
                }

                // Clean up headers and easy handle BEFORE signaling completion.
                // Once set_value() is called, the waiting thread in WaitForGet()
                // may immediately delete asyncOp, causing use-after-free.
                if (asyncOp->headers)
                {
                    curl_slist_free_all(asyncOp->headers);
                    asyncOp->headers = nullptr;
                }

                curl_multi_remove_handle(m_MultiHandle, easy);
                curl_easy_cleanup(easy);
                easy = nullptr;
                --m_ActiveHandles;

                asyncOp->promise.set_value(success);
            }

            if (easy)
            {
                curl_multi_remove_handle(m_MultiHandle, easy);
                curl_easy_cleanup(easy);
                --m_ActiveHandles;
            }
        }
    }
}

void CurlMultiPool::WorkerLoop()
{
    while (true)
    {
        // Drain pending queue into curl multi handle, up to MaxActiveHandles
        {
            std::unique_lock<std::mutex> lock(m_QueueMutex);

            while (!m_PendingQueue.empty() && m_ActiveHandles < MaxActiveHandles)
            {
                PendingSubmit req = m_PendingQueue.front();
                m_PendingQueue.pop_front();

                CURLMcode rc = curl_multi_add_handle(m_MultiHandle, req.easyHandle);
                if (rc != CURLM_OK)
                {
                    req.asyncOp->errorMsg =
                        std::string("curl_multi_add_handle failed: ") + curl_multi_strerror(rc);
                    req.asyncOp->promise.set_value(false);
                    if (req.asyncOp->headers)
                    {
                        curl_slist_free_all(req.asyncOp->headers);
                        req.asyncOp->headers = nullptr;
                    }
                    curl_easy_cleanup(req.easyHandle);
                }
                else
                {
                    ++m_ActiveHandles;
                }
            }
        }

        // Drive transfers and process completions
        int runningHandles = 0;
        curl_multi_perform(m_MultiHandle, &runningHandles);
        ProcessCompletedTransfers();

        // Check if we should block waiting for new work
        if (runningHandles == 0)
        {
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            if (!m_Running && m_PendingQueue.empty())
            {
                break;
            }
            if (m_PendingQueue.empty())
            {
                m_QueueCV.wait(lock);
            }
            continue;
        }

        // Wait for socket activity (up to 100ms)
        int numfds;
        curl_multi_wait(m_MultiHandle, nullptr, 0, 100, &numfds);
    }

    ProcessCompletedTransfers();
}

#endif // ADIOS2_HAVE_CURL

// ======================================================================
// XrootdHttpRemote implementation
// ======================================================================

XrootdHttpRemote::XrootdHttpRemote(const adios2::HostOptions &hostOptions) : Remote(hostOptions) {}

XrootdHttpRemote::~XrootdHttpRemote() { Close(); }

std::string XrootdHttpRemote::UrlEncode(const std::string &str)
{
    if (str.empty())
        return str;

    // Pure C++ URL encoding — no curl handle needed, fully thread-safe.
    std::ostringstream encoded;
    encoded.fill('0');
    encoded << std::hex;

    for (unsigned char c : str)
    {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
        {
            encoded << c;
        }
        else
        {
            encoded << '%' << std::uppercase << std::setw(2) << static_cast<int>(c)
                    << std::nouppercase;
        }
    }

    return encoded.str();
}

void XrootdHttpRemote::Open(const std::string hostname, const int32_t port,
                            const std::string filename, const Mode mode, bool RowMajorOrdering,
                            const Params &params)
{
#ifdef ADIOS2_HAVE_CURL
    // Ensure the CurlMultiPool singleton is constructed (and curl_global_init
    // called) before any easy handles are created. C++11 static local init
    // guarantees this is thread-safe even when Open() is called from worker
    // threads.
    CurlMultiPool::getInstance();
#endif

    m_Filename = filename;
    m_Mode = mode;
    m_RowMajorOrdering = RowMajorOrdering;

    auto it = params.find("UseHttps");
    if (it != params.end())
        m_UseHttps = (it->second == "true" || it->second == "1" || it->second == "yes");

    it = params.find("CAPath");
    if (it != params.end())
        m_CACertPath = it->second;

    it = params.find("VerifySSL");
    if (it != params.end())
        m_VerifySSL = (it->second == "true" || it->second == "1" || it->second == "yes");

    it = params.find("ConnectTimeout");
    if (it != params.end())
        m_ConnectTimeout = std::stol(it->second);

    it = params.find("RequestTimeout");
    if (it != params.end())
        m_RequestTimeout = std::stol(it->second);

    std::ostringstream urlStream;
    urlStream << (m_UseHttps ? "https" : "http") << "://" << hostname << ":" << port << "/ssi";
    m_BaseUrl = urlStream.str();

    // Note: helper::Log() uses a global unordered_set without locking,
    // so it is not safe to call from multiple threads concurrently.
    // Open() may be called from CampaignReader worker threads in parallel.

    m_OpenSuccess = true;
}

void XrootdHttpRemote::Close() { m_OpenSuccess = false; }

std::string XrootdHttpRemote::BuildRequestString(const char *VarName, size_t Step, size_t StepCount,
                                                 size_t BlockID, const Dims &Count,
                                                 const Dims &Start, const Accuracy &accuracy)
{
    std::ostringstream reqStream;
    std::string encodedFilename = UrlEncode(m_Filename);
    std::string encodedVarName = UrlEncode(std::string(VarName));

    reqStream << "get Filename=" << encodedFilename;
    reqStream << "&RMOrder=" << (m_RowMajorOrdering ? 1 : 0);
    reqStream << "&Varname=" << encodedVarName;
    reqStream << "&StepStart=" << Step;
    reqStream << "&StepCount=" << StepCount;
    reqStream << "&Block=" << BlockID;
    reqStream << "&Dims=" << Count.size();

    for (const auto &c : Count)
        reqStream << "&Count=" << c;
    for (const auto &s : Start)
        reqStream << "&Start=" << s;

    // Add accuracy parameters
    reqStream << "&AccuracyError=" << accuracy.error;
    reqStream << "&AccuracyNorm=" << accuracy.norm;
    reqStream << "&AccuracyRelative=" << (accuracy.relative ? 1 : 0);

    return reqStream.str();
}

CURL *XrootdHttpRemote::CreateEasyHandle(AsyncGet *asyncOp, const std::string &url,
                                         const std::string &postData)
{
#ifdef ADIOS2_HAVE_CURL
    CURL *easy = curl_easy_init();
    if (!easy)
        return nullptr;

    curl_easy_setopt(easy, CURLOPT_PRIVATE, asyncOp);
    asyncOp->easyHandle = easy;

    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, postData.c_str());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &asyncOp->responseData);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, m_ConnectTimeout);
    curl_easy_setopt(easy, CURLOPT_TIMEOUT, m_RequestTimeout);

    if (m_UseHttps)
    {
        if (!m_VerifySSL)
        {
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
        }
        else
        {
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
            if (!m_CACertPath.empty())
                curl_easy_setopt(easy, CURLOPT_CAINFO, m_CACertPath.c_str());
        }
    }

    asyncOp->headers =
        curl_slist_append(nullptr, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, asyncOp->headers);
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

    return easy;
#else
    return nullptr;
#endif
}

Remote::GetHandle XrootdHttpRemote::Get(const char *VarName, size_t Step, size_t StepCount,
                                        size_t BlockID, Dims &Count, Dims &Start,
                                        Accuracy &accuracy, void *dest)
{
    if (!m_OpenSuccess)
    {
        return nullptr;
    }

#ifdef ADIOS2_HAVE_CURL
    CurlMultiPool &pool = CurlMultiPool::getInstance();

    AsyncGet *asyncOp = new AsyncGet();
    asyncOp->destBuffer = dest;

    std::string postData =
        BuildRequestString(VarName, Step, StepCount, BlockID, Count, Start, accuracy);
    CURL *easy = CreateEasyHandle(asyncOp, m_BaseUrl, postData);
    if (!easy)
    {
        delete asyncOp;
        return nullptr;
    }

    pool.Submit(easy, asyncOp);
    return static_cast<GetHandle>(asyncOp);
#else
    return nullptr;
#endif
}

bool XrootdHttpRemote::WaitForGet(GetHandle handle)
{
    if (!handle)
        return false;

    AsyncGet *asyncOp = static_cast<AsyncGet *>(handle);
    bool result = asyncOp->promise.get_future().get();

    if (!result)
    {
        // Log to stderr directly — helper::Log() is not thread-safe
        std::cerr << "XrootdHttpRemote::WaitForGet failed: " << asyncOp->errorMsg << std::endl;
    }

    delete asyncOp;
    return result;
}

Remote::GetHandle XrootdHttpRemote::Read(size_t Start, size_t Size, void *Dest) { return nullptr; }

} // end namespace adios2
