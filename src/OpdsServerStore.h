#pragma once
#include <string>
#include <vector>

/**
 * OPDS (Open Publication Distribution System) server configuration.
 * 
 * Stores connection details for e-book catalog servers.
 * Passwords are obfuscated in memory and encrypted at rest on disk using
 * the device's unique hardware MAC address as the obfuscation key.
 */
struct OpdsServer {
  std::string name;       /**< Display name for the server */
  std::string url;        /**< Base URL of the OPDS feed */
  std::string username;   /**< Authentication username (optional) */
  std::string password;   /**< Authentication password (plaintext in RAM, encrypted on disk) */
};

class OpdsServerStore;
namespace JsonSettingsIO {
bool saveOpds(const OpdsServerStore& store, const char* path);
bool loadOpds(OpdsServerStore& store, const char* json, bool* needsResave);
}  // namespace JsonSettingsIO

/**
 * Singleton class for storing OPDS server configurations on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON.
 */
class OpdsServerStore {
 private:
  static OpdsServerStore instance;
  std::vector<OpdsServer> servers;

  /* Maximum OPDS servers to store (limit prevents excessive memory usage) */
  static constexpr size_t MAX_SERVERS = 8;

  /* Private singleton constructor */
  OpdsServerStore() = default;

  friend bool JsonSettingsIO::saveOpds(const OpdsServerStore&, const char*);
  friend bool JsonSettingsIO::loadOpds(OpdsServerStore&, const char*, bool*);

 public:
  /* Prevent copying of singleton instance */
  OpdsServerStore(const OpdsServerStore&) = delete;
  OpdsServerStore& operator=(const OpdsServerStore&) = delete;

  /**
   * Get the singleton instance of the OPDS server store.
   */
  static OpdsServerStore& getInstance() { return instance; }

  /**
   * Save all configured servers to opds.json on the SD card.
   * Passwords are encrypted using the device MAC address.
   * 
   * @return true if save succeeded, false on I/O error
   */
  bool saveToFile() const;

  /**
   * Load OPDS server configurations from opds.json.
   * Automatically triggers migration from legacy settings if needed.
   * 
   * @return true if load succeeded, false on I/O error or corrupted data
   */
  bool loadFromFile();

  /**
   * Add a new OPDS server to the store.
   * 
   * @param server Server configuration to add
   * @return true if added successfully, false if store is full
   */
  bool addServer(const OpdsServer& server);

  /**
   * Update an existing OPDS server configuration.
   * 
   * @param index Zero-based index of server to update
   * @param server New server configuration
   * @return true if updated successfully, false if index is out of bounds
   */
  bool updateServer(size_t index, const OpdsServer& server);

  /**
   * Remove an OPDS server from the store.
   * 
   * @param index Zero-based index of server to remove
   * @return true if removed successfully, false if index is out of bounds
   */
  bool removeServer(size_t index);

  /**
   * Get all configured OPDS servers (read-only).
   */
  const std::vector<OpdsServer>& getServers() const { return servers; }

  /**
   * Get a specific server by index.
   * 
   * @param index Zero-based server index
   * @return Pointer to server config, or nullptr if index is out of bounds
   */
  const OpdsServer* getServer(size_t index) const;

  /**
   * Get the number of configured OPDS servers.
   */
  size_t getCount() const { return servers.size(); }

  /**
   * Check whether any OPDS servers are configured.
   */
  bool hasServers() const { return !servers.empty(); }

  /**
   * Migrate OPDS configuration from legacy CrossPointSettings.
   * Only called once during initialization if no opds.json exists.
   * 
   * @return true if migration succeeded or was not needed
   */
  bool migrateFromSettings();
};

#define OPDS_STORE OpdsServerStore::getInstance()
