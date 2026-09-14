#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <vector>

/**
 * WiFi network credential (SSID and password).
 * 
 * Passwords are stored plaintext in memory for quick access.
 * On disk (wifis.json), they are encrypted using the device's unique
 * hardware MAC address as the obfuscation key and base64-encoded.
 */
struct WifiCredential {
  std::string ssid;       /**< Network name */
  std::string password;   /**< Network password (plaintext in RAM, encrypted on disk) */
};

class WifiCredentialStore;
namespace JsonSettingsIO {
bool saveWifi(const WifiCredentialStore& store, const char* path);
bool loadWifi(WifiCredentialStore& store, const char* json, bool* needsResave);
}  // namespace JsonSettingsIO

/**
 * Singleton class for managing WiFi network credentials.
 * 
 * Securely stores WiFi credentials on the SD card with per-device encryption.
 * Features:
 * - Per-device hardware-based obfuscation (not cryptographically secure but device-bound)
 * - Thread-safe access via mutexes (separate mutexes for credentials and persistence)
 * - Atomic JSON save/load workflow
 * - Legacy binary format migration support
 * - Last-connected SSID tracking for quick reconnection
 * 
 * Thread Safety:
 * - credentialMutex protects in-memory credential vector
 * - persistenceMutex serializes SD card I/O operations
 * - Locks are released before SD card operations to prevent deadlock
 */
class WifiCredentialStore {
 private:
  static WifiCredentialStore instance;
  std::vector<WifiCredential> credentials;
  std::string lastConnectedSsid;

  /* Protects in-memory credential vector during access */
  mutable std::mutex credentialMutex;

  /* Serializes SD card I/O operations to prevent concurrent read/write conflicts */
  mutable std::mutex persistenceMutex;

  /* Maximum number of WiFi networks to store (memory constraint) */
  static constexpr size_t MAX_NETWORKS = 8;

  /* Maximum password length per credential (buffer overflow protection) */
  static constexpr size_t MAX_PASSWORD_LENGTH = 64;

  /* Private singleton constructor */
  WifiCredentialStore() = default;

  /* Internal save without mutex (for use within locked regions) */
  bool saveToFileUnlocked() const;

  /* Load credentials from legacy binary wifis.bak format */
  bool loadFromBinaryFile();

  friend bool JsonSettingsIO::saveWifi(const WifiCredentialStore&, const char*);
  friend bool JsonSettingsIO::loadWifi(WifiCredentialStore&, const char*, bool*);

 public:
  /* Prevent copying of singleton instance */
  WifiCredentialStore(const WifiCredentialStore&) = delete;
  WifiCredentialStore& operator=(const WifiCredentialStore&) = delete;

  /**
   * Get the singleton instance of the WiFi credential store.
   */
  static WifiCredentialStore& getInstance() { return instance; }

  /**
   * Save all WiFi credentials to wifis.json on the SD card.
   * Passwords are encrypted using the device MAC address.
   * 
   * @return true if save succeeded, false on I/O error or full storage
   */
  bool saveToFile() const;

  /**
   * Load WiFi credentials from wifis.json on the SD card.
   * Falls back to legacy binary format if JSON doesn't exist.
   * 
   * @return true if load succeeded, false on I/O error or corrupted data
   */
  bool loadFromFile();

  /**
   * Add a new WiFi network credential.
   * 
   * @param ssid Network SSID
   * @param password Network password (max 64 characters)
   * @return true if added successfully, false if store is full or SSID already exists
   */
  bool addCredential(const std::string& ssid, const std::string& password);

  /**
   * Remove a saved WiFi credential by SSID.
   * 
   * @param ssid SSID to remove
   * @return true if removed successfully, false if not found
   */
  bool removeCredential(const std::string& ssid);

  /**
   * Look up a WiFi credential by SSID.
   * 
   * @param ssid Network SSID to search for
   * @return Credential structure if found, std::nullopt if not found
   */
  std::optional<WifiCredential> findCredential(const std::string& ssid) const;

  /**
   * Check whether any WiFi credentials are stored.
   * Used by auto-connect to determine if network connection is possible.
   * 
   * @return true if at least one credential exists, false otherwise
   */
  bool hasCredentials() const;

  /**
   * Check whether a specific WiFi network has a saved credential.
   * 
   * @param ssid Network SSID to check
   * @return true if credential exists, false otherwise
   */
  bool hasSavedCredential(const std::string& ssid) const;

  /**
   * Record the SSID of the last successfully connected network.
   * Used to resume connection on device wake-up.
   * 
   * @param ssid SSID to remember
   */
  void setLastConnectedSsid(const std::string& ssid);

  /**
   * Get the SSID of the last successfully connected network.
   * 
   * @return Last connected SSID, or empty string if not set
   */
  std::string getLastConnectedSsid() const;

  /**
   * Clear the last-connected SSID record.
   * Called when user explicitly disconnects or manually selects a different network.
   */
  void clearLastConnectedSsid();

  /**
   * Remove all stored WiFi credentials.
   * Used when factory-resetting or migrating to a new device.
   */
  void clearAll();
};

// Helper macro to access credentials store
#define WIFI_STORE WifiCredentialStore::getInstance()
