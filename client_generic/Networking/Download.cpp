#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <cstdio>
#include <string>

#include "Log.h"
#include "Networking.h"

namespace Network
{

/*
        CFileDownloader().
        Constructor.
*/
CFileDownloader::CFileDownloader(const std::string& _name)
    : CCurlTransfer(_name), m_Data()
{
    m_Data.reserve(10 * 1024 * 1024);
}

/*
        ~CFileDownloader().
        Destructor.
*/
CFileDownloader::~CFileDownloader() {}

/*
        customWrite().
        Appends incoming data to string.
*/
int32_t CFileDownloader::customWrite(void* _pBuffer, size_t _size,
                                     size_t _nmemb, void* _pUserData)
{
    CFileDownloader* pOut = (CFileDownloader*)_pUserData;
    if (!pOut)
    {
        g_Log->Info("Error, no _pUserData.");
        return -1;
    }

    pOut->m_Data.append((char*)_pBuffer, _size * _nmemb);
    return (int32_t)(_size * _nmemb);
}

int32_t CFileDownloader::writeToFile(void* _pBuffer, size_t _size,
                                     size_t _nmemb, void* _pUserData)
{
    CFileDownloader* pOut = static_cast<CFileDownloader*>(_pUserData);
    if (!pOut || !pOut->m_pOutFile)
        return -1;
    size_t bytes = _size * _nmemb;
    size_t written = fwrite(_pBuffer, 1, bytes, pOut->m_pOutFile);
    if (pOut->m_md5Active)
        MD5_Update(&pOut->m_md5Ctx, _pBuffer, bytes);
    return static_cast<int32_t>(written);
}

bool CFileDownloader::PerformToFile(const std::string& _url,
                                    const std::string& _destPath)
{
    m_pOutFile = fopen(_destPath.c_str(), "wb");
    if (!m_pOutFile)
    {
        g_Log->Error("PerformToFile: cannot open '%s' for writing", _destPath.c_str());
        return false;
    }

    m_ResponseHeaders.clear();
    m_md5Hex.clear();
    MD5_Init(&m_md5Ctx);
    m_md5Active = true;

    bool ok = false;
    if (Verify(curl_easy_setopt(m_pCurl, CURLOPT_WRITEDATA, this)) &&
        Verify(curl_easy_setopt(m_pCurl, CURLOPT_WRITEFUNCTION,
                                &CFileDownloader::writeToFile)) &&
        Verify(curl_easy_setopt(m_pCurl, CURLOPT_HEADERFUNCTION,
                                &CFileDownloader::headerCallback)) &&
        Verify(curl_easy_setopt(m_pCurl, CURLOPT_HEADERDATA, this)))
    {
        ok = CCurlTransfer::Perform(_url);
    }

    m_md5Active = false;
    fclose(m_pOutFile);
    m_pOutFile = nullptr;

    if (ok)
    {
        unsigned char digest[MD5_DIGEST_LENGTH];
        MD5_Final(digest, &m_md5Ctx);
        std::ostringstream ss;
        for (int i = 0; i < MD5_DIGEST_LENGTH; ++i)
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
        m_md5Hex = ss.str();
    }
    return ok;
}

bool CFileDownloader::SetPostFields(const char* postFields)
{
    if (!Verify(curl_easy_setopt(m_pCurl, CURLOPT_POSTFIELDS, postFields)))
        return false;
    return true;
}

/*
        Perform().
        Download specific Perform function.
*/
bool CFileDownloader::Perform(const std::string& _url)
{
    m_Data = "";
    m_ResponseHeaders.clear();

    if (!Verify(curl_easy_setopt(m_pCurl, CURLOPT_WRITEDATA, this)))
        return false;
    if (!Verify(curl_easy_setopt(m_pCurl, CURLOPT_WRITEFUNCTION,
                                 &CFileDownloader::customWrite)))
        return false;
    if (!Verify(curl_easy_setopt(m_pCurl, CURLOPT_HEADERFUNCTION,
                                 &CFileDownloader::headerCallback)))
        return false;
    if (!Verify(curl_easy_setopt(m_pCurl, CURLOPT_HEADERDATA, this)))
        return false;
    
    return CCurlTransfer::Perform(_url);
}

std::string CFileDownloader::GetResponseHeader(const std::string& headerName) const
{
    std::string lowerHeaderName = headerName;
    std::transform(lowerHeaderName.begin(), lowerHeaderName.end(), lowerHeaderName.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    
    auto it = m_ResponseHeaders.find(lowerHeaderName);
    if (it != m_ResponseHeaders.end())
    {
        return it->second;
    }
    return "";
}

std::vector<std::string> CFileDownloader::GetResponseHeaders(const std::string& headerName) const
{
    std::string lowerHeaderName = headerName;
    std::transform(lowerHeaderName.begin(), lowerHeaderName.end(), lowerHeaderName.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    
    std::vector<std::string> headers;
    auto range = m_ResponseHeaders.equal_range(lowerHeaderName);
    for (auto it = range.first; it != range.second; ++it)
    {
        headers.push_back(it->second);
    }
    return headers;
}

size_t CFileDownloader::headerCallback(char* buffer, size_t size, size_t nitems, void* userdata)
{
    size_t realsize = size * nitems;
    CFileDownloader* downloader = static_cast<CFileDownloader*>(userdata);
    
    std::string header(buffer, realsize);
    size_t colonPos = header.find(':');
    if (colonPos != std::string::npos)
    {
        std::string key = header.substr(0, colonPos);
        std::string value = header.substr(colonPos + 1);
        
        // Trim whitespace
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        
        // Convert key to lowercase for case-insensitive comparison
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        
        downloader->m_ResponseHeaders.insert(std::make_pair(key, value));
    }
    
    return realsize;
}
/*
        Save().
        Saves completed data to file.
*/
bool CFileDownloader::Save(const std::string& _output)
{
    std::ofstream out(_output.c_str(), std::ios::out | std::ios::binary);
    if (out.bad())
    {
        g_Log->Info("Failed to open output file.");
        return false;
    }

    out << m_Data;
    out.close();

    g_Log->Info("%s saved.", _output.c_str());
    return true;
}

/*
        CFileDownloader_TimeCondition().
        Constructor.
*/
CFileDownloader_TimeCondition::CFileDownloader_TimeCondition(
    const std::string& _name)
    : CFileDownloader(_name)
{
}

/*
        ~CFileDownloader_TimeCondition
        Destructor.
*/
CFileDownloader_TimeCondition::~CFileDownloader_TimeCondition() {}

/*
        Perform().

*/
bool CFileDownloader_TimeCondition::PerformDownloadWithTC(
    const std::string& _url, const time_t _lastTime)
{
    if (!Verify(curl_easy_setopt(m_pCurl, CURLOPT_TIMECONDITION, this)))
        return false;
    if (!Verify(curl_easy_setopt(m_pCurl, CURLOPT_TIMEVALUE, _lastTime)))
        return false;
    return CFileDownloader::Perform(_url);
}

}; // namespace Network
