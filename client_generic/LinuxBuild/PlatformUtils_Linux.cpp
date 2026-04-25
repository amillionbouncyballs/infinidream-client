//
//  PlatformUtils_Linux.cpp
//  infinidream — Linux platform utilities
//

#include "PlatformUtils.h"
#include "PlatformUtils_Internal.h"
#include "Log.h"
#include "clientversion.h"
#include <boost/json.hpp>
#include <atomic>
#include <curl/curl.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <netinet/in.h>
#include <openssl/md5.h>
#include <pthread.h>
#include <queue>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

// ---------------------------------------------------------------------------
// Internet reachability — try a non-blocking connect to 8.8.8.8:53
// ---------------------------------------------------------------------------
bool PlatformUtils::IsInternetReachable()
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return false;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr);

    connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(sock, &writefds);
    struct timeval tv{2, 0};

    int result = select(sock + 1, nullptr, &writefds, nullptr, &tv);
    close(sock);
    return result > 0;
}

// ---------------------------------------------------------------------------
// BuildData.json helpers
// ---------------------------------------------------------------------------
static std::string ReadBuildDataValue(const std::string& key)
{
    std::string exePath = PlatformUtils::GetAppPath();
    auto pos = exePath.rfind('/');
    std::string dir = (pos != std::string::npos) ? exePath.substr(0, pos + 1) : "./";

    std::ifstream f(dir + "BuildData.json");
    if (!f.is_open())
        f.open("BuildData.json");
    if (!f.is_open())
        return "";

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    try
    {
        auto val = boost::json::parse(content);
        if (!val.is_object())
            return "";
        const auto& obj = val.as_object();
        auto it = obj.find(key);
        if (it == obj.end() || !it->value().is_string())
            return "";
        return std::string(it->value().as_string());
    }
    catch (...)
    {
        return "";
    }
}

std::string PlatformUtils::GetBuildDate()   { return ReadBuildDataValue("BUILD_DATE"); }
std::string PlatformUtils::GetGitRevision() { return ReadBuildDataValue("REVISION"); }

std::string PlatformUtils::GetAppVersion()
{
    std::string v = ReadBuildDataValue("VERSION");
    return v.empty() ? "0.0.0" : v;
}

std::string PlatformUtils::GetPlatformName() { return "Linux"; }

// ---------------------------------------------------------------------------
// App / working directory
// ---------------------------------------------------------------------------
std::string PlatformUtils::GetAppPath()
{
    char buf[4096] = {};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0)
        buf[len] = '\0';
    return std::string(buf);
}

std::string PlatformUtils::GetWorkingDir()
{
    std::string exe = GetAppPath();
    auto pos = exe.rfind('/');
    return (pos != std::string::npos) ? exe.substr(0, pos + 1) : "./";
}

// ---------------------------------------------------------------------------
// URL, cursor, mouse
// ---------------------------------------------------------------------------
void PlatformUtils::OpenURLExternally(std::string_view _url)
{
    std::string url(_url);
    const char* args[] = { "xdg-open", url.c_str(), nullptr };
    pid_t pid;
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr,
                     const_cast<char* const*>(args), environ) == 0)
        waitpid(pid, nullptr, WNOHANG); // reap without blocking
}

static bool s_cursorHidden = false;

void PlatformUtils::SetCursorHidden(bool _hidden)
{
    s_cursorHidden = _hidden;
    // Actual X11 cursor manipulation is done in CDisplayVulkan
}

bool PlatformUtils_GetCursorHidden() { return s_cursorHidden; }

static std::function<void(int, int)> s_mouseCallback;

void PlatformUtils::SetOnMouseMovedCallback(std::function<void(int, int)> _callback)
{
    s_mouseCallback = std::move(_callback);
}

std::function<void(int, int)>& PlatformUtils_GetMouseCallback()
{
    return s_mouseCallback;
}

// ---------------------------------------------------------------------------
// Thread name
// ---------------------------------------------------------------------------
void PlatformUtils::SetThreadName(std::string_view _name)
{
    std::string name(_name);
    if (name.size() > 15)
        name = name.substr(0, 15); // pthread limit
    pthread_setname_np(pthread_self(), name.c_str());
}

// ---------------------------------------------------------------------------
// Main-thread dispatch queue
// ---------------------------------------------------------------------------
static std::mutex s_dispatchMutex;
static std::queue<std::function<void()>> s_dispatchQueue;

void PlatformUtils::DispatchOnMainThread(std::function<void()> _func)
{
    std::lock_guard<std::mutex> lock(s_dispatchMutex);
    s_dispatchQueue.push(std::move(_func));
}

void PlatformUtils_DrainMainThreadQueue()
{
    std::queue<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lock(s_dispatchMutex);
        std::swap(local, s_dispatchQueue);
    }
    while (!local.empty())
    {
        local.front()();
        local.pop();
    }
}

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------
void PlatformUtils::NotifyError(std::string_view errorMessage)
{
    fprintf(stderr, "[ERROR] %.*s\n",
            static_cast<int>(errorMessage.size()), errorMessage.data());
}

// ---------------------------------------------------------------------------
// MD5 (identical to Mac implementation — OpenSSL)
// ---------------------------------------------------------------------------
std::string PlatformUtils::CalculateFileMD5(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file)
        return "";

    MD5_CTX context;
    MD5_Init(&context);

    char buffer[4096];
    while (file.read(buffer, sizeof(buffer)))
        MD5_Update(&context, buffer, static_cast<unsigned long>(file.gcount()));
    if (file.gcount() > 0)
        MD5_Update(&context, buffer, static_cast<unsigned long>(file.gcount()));

    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_Final(digest, &context);

    std::ostringstream ss;
    for (int i = 0; i < MD5_DIGEST_LENGTH; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(digest[i]);
    return ss.str();
}

// ---------------------------------------------------------------------------
// CDelayedDispatch
// ---------------------------------------------------------------------------
CDelayedDispatch::CDelayedDispatch(std::function<void()> _func)
    : m_DispatchTime(0), m_Func(std::move(_func))
{}

void CDelayedDispatch::Cancel()
{
    m_DispatchTime = 0;
}

void CDelayedDispatch::DispatchAfter(uint64_t seconds)
{
    uint64_t dispatchTime = static_cast<uint64_t>(time(nullptr)) + seconds;
    m_DispatchTime        = dispatchTime;

    std::thread([this, dispatchTime, seconds]() {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        if (m_DispatchTime == dispatchTime)
            PlatformUtils::DispatchOnMainThread(m_Func);
    }).detach();
}

// ---------------------------------------------------------------------------
// Linux auto-update: fetch the Sparkle appcast and compare versions.
// ---------------------------------------------------------------------------
static std::atomic<bool> s_updateAvailable{false};

bool ESScreensaver_IsUpdateAvailable(void)
{
    return s_updateAvailable.load();
}

static size_t AppcastWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// Returns {major, minor, patch} parsed from "X.Y.Z" (or {0,0,0} on failure).
static std::tuple<int,int,int> ParseVersion(const std::string& v)
{
    int ma = 0, mi = 0, pa = 0;
    sscanf(v.c_str(), "%d.%d.%d", &ma, &mi, &pa);
    return {ma, mi, pa};
}

static void RunUpdateCheck()
{
    const std::string appcastUrl =
        "https://infinidream.ai/alpha/appcast.xml";

    CURL* curl = curl_easy_init();
    if (!curl)
        return;

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, appcastUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AppcastWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        g_Log->Info("Update check failed: %s", curl_easy_strerror(res));
        return;
    }

    // Extract <sparkle:shortVersionString>X.Y.Z</sparkle:shortVersionString>
    const std::string tag = "<sparkle:shortVersionString>";
    auto pos = body.find(tag);
    if (pos == std::string::npos)
        return;
    pos += tag.size();
    auto end = body.find('<', pos);
    if (end == std::string::npos)
        return;
    std::string remoteVer = body.substr(pos, end - pos);

    const std::string localVer = std::string(VER_MAJOR) + "." + VER_MINOR + "." + VER_BUILD;
    auto [rMa, rMi, rPa] = ParseVersion(remoteVer);
    auto [lMa, lMi, lPa] = ParseVersion(localVer);

    bool newer = std::tie(rMa, rMi, rPa) > std::tie(lMa, lMi, lPa);
    if (newer)
    {
        g_Log->Info("Update available: local=%s remote=%s",
                    localVer.c_str(), remoteVer.c_str());
        s_updateAvailable.store(true);
    }
    else
    {
        g_Log->Info("Up to date (local=%s remote=%s)",
                    localVer.c_str(), remoteVer.c_str());
    }
}

void ESLinux_StartUpdateCheck(void)
{
    std::thread(RunUpdateCheck).detach();
}
