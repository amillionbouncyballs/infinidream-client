#ifdef WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#include "Log.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace Base
{

bool GetFileList(std::vector<std::string>& _list, const std::string _dir,
                 const std::string _extension, const bool _usegoldsheep,
                 const bool _usefreesheep, const bool _scanProps)
{
    bool gotSheep = false;
    try
    {
        fs::path p(_dir);

        for (const auto& entry : fs::directory_iterator(p))
        {
            if (entry.is_directory())
            {
                gotSheep |= GetFileList(
                    _list, entry.path().string() + "/",
                    _extension, _usegoldsheep, _usefreesheep, _scanProps);
            }
            else
            {
                std::string fname = entry.path().filename().string();
                std::string ext   = entry.path().extension().string();

                if (ext == _extension)
                {
                    if (_scanProps)
                    {
                        int generation, id, first, last;
                        if (4 == sscanf(fname.c_str(),
                                        (std::string("%d=%d=%d=%d.") + _extension).c_str(),
                                        &generation, &id, &first, &last))
                        {
                            std::string xxxname(fname);
                            xxxname.replace(fname.size() - 3, 3, "xxx");

                            if (!fs::exists(p / xxxname))
                            {
                                if ((_usegoldsheep && generation >= 10000) ||
                                    (_usefreesheep && generation < 10000))
                                {
                                    _list.push_back(entry.path().string());
                                    gotSheep = true;
                                }
                            }
                        }
                    }
                    else
                    {
                        _list.push_back(entry.path().string());
                        gotSheep = true;
                    }
                }
            }
        }
    }
    catch (fs::filesystem_error& err)
    {
        g_Log->Error("Path enumeration threw error: %s", err.what());
    }

    return gotSheep;
}

} // namespace Base
