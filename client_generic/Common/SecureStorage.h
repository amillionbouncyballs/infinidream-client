#pragma once
#include <string>

// Platform-abstracted storage for secrets (session tokens, API keys).
// On Linux: kernel keyring (no external dependencies, survives app restarts within session).
// On Mac: plaintext settings fallback (Keychain integration requires Security.framework in Xcode project).
// Fallback: g_Settings() plaintext JSON — used for migration and unsupported platforms.
namespace SecureStorage
{
    // Read a secret. Checks OS keyring first; falls back to plaintext settings for migration.
    std::string Get(const std::string& settingsKey, const std::string& defaultVal = "");

    // Write a secret to the OS keyring. On success, clears the plaintext settings entry.
    // Passing an empty value clears the secret from both keyring and plaintext settings.
    void Set(const std::string& settingsKey, const std::string& value);
}
