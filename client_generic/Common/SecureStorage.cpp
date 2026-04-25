#include "SecureStorage.h"
#include "Settings.h"
#include "Log.h"
#include <string>

// -----------------------------------------------------------------------
// Linux: kernel keyring via raw syscalls — no extra library dependencies.
// Keys are stored in the persistent keyring (survives logouts on systemd
// distros) or the user session keyring as a fallback.
// -----------------------------------------------------------------------
#ifdef LINUX_GNU
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

using key_serial_t = int32_t;
static constexpr key_serial_t kKeySpecUserKeyring       = -4;
static constexpr key_serial_t kKeySpecPersistentKeyring = -10;
static constexpr long         kKeyctlRead               = 11;
static constexpr long         kKeyctlInvalidate         = 21;

static key_serial_t sKeyring = 0;

static key_serial_t ChooseKeyring()
{
    // Probe persistent keyring; fall back to user session keyring.
    key_serial_t k = (key_serial_t)syscall(SYS_add_key, "user",
        "infinidream:_probe", "", 0, kKeySpecPersistentKeyring);
    if (k > 0) {
        syscall(SYS_keyctl, kKeyctlInvalidate, (long)k, 0L, 0L, 0L);
        return kKeySpecPersistentKeyring;
    }
    return kKeySpecUserKeyring;
}

static key_serial_t GetKeyring()
{
    if (sKeyring == 0)
        sKeyring = ChooseKeyring();
    return sKeyring;
}

static std::string KeyringKeyFor(const std::string& settingsKey)
{
    // Strip "settings.content." prefix for brevity; prefix with app name.
    const std::string prefix = "settings.content.";
    const std::string suffix = settingsKey.substr(
        settingsKey.size() >= prefix.size() &&
            settingsKey.substr(0, prefix.size()) == prefix
            ? prefix.size() : 0);
    return "infinidream:" + suffix;
}

static bool LinuxKeyringSet(const std::string& desc, const std::string& value)
{
    key_serial_t k = (key_serial_t)syscall(SYS_add_key, "user",
        desc.c_str(), value.data(), value.size(), GetKeyring());
    return k > 0;
}

static std::string LinuxKeyringGet(const std::string& desc)
{
    key_serial_t k = (key_serial_t)syscall(SYS_request_key, "user",
        desc.c_str(), nullptr, GetKeyring());
    if (k <= 0)
        return {};

    char buf[8192];
    ssize_t n = syscall(SYS_keyctl, kKeyctlRead, (long)k,
                        (long)buf, (long)sizeof(buf), 0L);
    if (n <= 0)
        return {};
    return std::string(buf, static_cast<size_t>(n));
}

static void LinuxKeyringClear(const std::string& desc)
{
    key_serial_t k = (key_serial_t)syscall(SYS_request_key, "user",
        desc.c_str(), nullptr, GetKeyring());
    if (k > 0)
        syscall(SYS_keyctl, kKeyctlInvalidate, (long)k, 0L, 0L, 0L);
}

#endif // LINUX_GNU

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------
namespace SecureStorage
{

std::string Get(const std::string& settingsKey, const std::string& defaultVal)
{
#ifdef LINUX_GNU
    const std::string kkey = KeyringKeyFor(settingsKey);
    std::string val = LinuxKeyringGet(kkey);
    if (!val.empty())
        return val;

    // Fall back to plaintext settings (migration path).
    val = g_Settings()->Get(settingsKey, std::string(""));
    if (!val.empty()) {
        // Opportunistically migrate to keyring and wipe from plaintext.
        if (LinuxKeyringSet(kkey, val))
            g_Settings()->Set(settingsKey, std::string(""));
    }
    return val.empty() ? defaultVal : val;
#else
    // Mac/Windows: read from plaintext settings until platform keyring is wired up.
    return g_Settings()->Get(settingsKey, defaultVal);
#endif
}

void Set(const std::string& settingsKey, const std::string& value)
{
#ifdef LINUX_GNU
    const std::string kkey = KeyringKeyFor(settingsKey);
    if (value.empty()) {
        LinuxKeyringClear(kkey);
        g_Settings()->Set(settingsKey, std::string(""));
        return;
    }
    if (LinuxKeyringSet(kkey, value)) {
        // Secret is in the keyring — clear the plaintext copy.
        g_Settings()->Set(settingsKey, std::string(""));
    } else {
        g_Log->Warning("SecureStorage: kernel keyring unavailable, storing in plaintext settings");
        g_Settings()->Set(settingsKey, value);
    }
#else
    g_Settings()->Set(settingsKey, value);
#endif
}

} // namespace SecureStorage
