#include <cstddef>
#include <cstdio>
#include <string>
#include <sys/types.h>
#if defined(WIN32) && !defined(_MSC_VER)
#include <dirent.h>
#endif
#include <cstring>
#ifdef WIN32
#include <process.h>
#include <windows.h>
#endif
#include <float.h>
#include <signal.h>

#ifdef LINUX_GNU
#include <execinfo.h>
#include <unistd.h>
#endif

#include "client.h"

#ifdef WIN32
#include "client_win32.h"
typedef CElectricSheep_Win32 CElectricSheepClient;
#else
#ifdef MAC
#include "client_mac.h"
#include <GLUT/glut.h>
#include <OpenGL/gl.h>
typedef CElectricSheep_Mac CElectricSheepClient;
#else  // Linux
#include "client_linux.h"
typedef CElectricSheep_Linux CElectricSheepClient;
#endif
#endif

#ifdef LINUX_GNU
static void crashHandler(int sig)
{
    // Write a backtrace to stderr and the log file using only async-signal-safe calls.
    const char* sigName = (sig == SIGSEGV) ? "SIGSEGV" : (sig == SIGABRT) ? "SIGABRT" : "signal";
    char header[128];
    int len = snprintf(header, sizeof(header), "\n[CRASH] Caught %s — backtrace:\n", sigName);
    write(STDERR_FILENO, header, static_cast<size_t>(len));

    void* frames[64];
    int nFrames = backtrace(frames, 64);
    backtrace_symbols_fd(frames, nFrames, STDERR_FILENO);

    // Re-raise with default handler so the process exits with the right status
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

//
#ifdef WIN32
int32_t APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                         LPSTR lpCmdLine, int nCmdShow)
{
#else
int32_t main(int argc, char* argv[])
{
#ifdef LINUX_GNU
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
    signal(SIGPIPE, SIG_IGN); // prevent SIGPIPE crash on broken network connections
#endif

    // Parse our flags before glutInit so GLUT doesn't interfere.
    bool cachedOnlyMode = false;
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--cached") == 0) { cachedOnlyMode = true; break; }

#if defined(MAC) || (defined(USE_GLUT) && !defined(WIN32))
    glutInit(&argc, argv);
#endif
#endif

    //	Start log (unattached).
    g_Log->Startup();

    CElectricSheepClient client;
    client.SetCachedOnlyMode(cachedOnlyMode);

    if (client.Startup())
        client.Run();

    //    g_Log->Info( "Raising access violation...\n" );
    //    asm( "movl $0, %eax" );
    //    asm( "movl $1, (%eax)" );

    //    __asm("int3");

    client.Shutdown();

    //g_Log->Shutdown();

    return 0;
}
