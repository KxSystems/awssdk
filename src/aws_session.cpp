#include <cctype>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>

#include <aws/core/platform/FileSystem.h>

#include "aws_session.h"
#include "s3_client.h"
#include "retired.h"
#include "s3_transfer.h"
#include "utils.h"

namespace awssdk {

const std::map<std::string, Aws::Utils::Logging::LogLevel> log_level_mapping = {
  {"OFF", Aws::Utils::Logging::LogLevel::Off},
  {"FATAL", Aws::Utils::Logging::LogLevel::Fatal},
  {"ERROR", Aws::Utils::Logging::LogLevel::Error},
  {"WARN", Aws::Utils::Logging::LogLevel::Warn},
  {"INFO", Aws::Utils::Logging::LogLevel::Info},
  {"DEBUG", Aws::Utils::Logging::LogLevel::Debug},
  {"TRACE", Aws::Utils::Logging::LogLevel::Trace}
};

static bool parse_log_level(K options, Aws::Utils::Logging::LogLevel & log_level) {
  std::string level;
  switch (dict_find_str(options, "loglevel", level)) {
    case DictLookup::Absent:
      return true;
    case DictLookup::WrongType:
      return false;
    case DictLookup::Found:
      break;
  }

  for (char & c : level) { c = (char)std::toupper((unsigned char)c); }
  const auto level_it = log_level_mapping.find(level);
  if (level_it == log_level_mapping.end()) {
    return false;
  }
  log_level = level_it->second;
  return true;
}

static void shut_down_at_exit() {
  // If SDK threads are still busy, the SDK is left running: the process is
  // ending anyway, and tearing it down under them would be worse.
  try {
    AwsSession::getInstance().shutDown();
  } catch (const std::exception &) {
  }
}

bool AwsSession::initialize(K options_k) {
  if (initialized) return false;
  Aws::Utils::Logging::LogLevel log_level = Aws::Utils::Logging::LogLevel::Info;
  if (!parse_log_level(options_k, log_level)) {
    throw std::invalid_argument("loglevel");
  }
  options.loggingOptions.logLevel = log_level;
  if (log_level != Aws::Utils::Logging::LogLevel::Off) {
    // in case it was initialized and shut down before
    log_prefix.clear();
    options.loggingOptions.defaultLogPrefix = Aws::DEFAULT_LOG_PREFIX;

    switch (dict_find_str(options_k, "logPrefix", log_prefix)) {
      case DictLookup::Absent:
        break;
      case DictLookup::WrongType:
        throw std::invalid_argument("type");
      case DictLookup::Found:
        const auto sep = log_prefix.find_last_of("/\\");
        if (sep != std::string::npos) {
          const std::string dir = log_prefix.substr(0, sep);
          if (!Aws::FileSystem::CreateDirectoryIfNotExists(dir.c_str(), true)) {
            throw std::runtime_error("logPrefix");
          }
        }
        options.loggingOptions.defaultLogPrefix = log_prefix.c_str();
        break;
    }
  }
  Aws::InitAPI(options);
  initialized = true;
  if (!atexit_registered) {
    std::atexit(&shut_down_at_exit);
    atexit_registered = true;
  }
  return true;
}

// Must not run while another call into the module is in progress. Every entry
// point runs on q's main thread, so none can. The SDK's own threads can still
// be running async transfers, so those are canceled and waited for first: no
// SDK work may outlive ShutdownAPI.
bool AwsSession::shutDown() {
  if (!initialized) return false;
  // Handles hold SDK resources, so they have to go before ShutdownAPI.
  // Transfer managers first: each one holds a client, which would otherwise
  // outlive the client sweep and be destroyed after ShutdownAPI.
  cancel_all_async_transfers();
  destroy_all_transfer_managers();
  destroy_all_clients();
  // The sweeps hand their objects over to be retired; SDK threads may still
  // hold them. Every one has to be destroyed, here, before ShutdownAPI: an SDK
  // thread still holding one is still doing SDK work. If they are not all done
  // in time, the SDK is left initialized and the caller told, so that a later
  // shutDown can finish the job. The swept handles stay inert either way.
  if (!drain_retired(std::chrono::seconds(10))) {
    throw std::runtime_error("busy");
  }
  Aws::ShutdownAPI(options);
  initialized = false;
  return true;
}

bool AwsSession::isInitialized() const {
  return initialized;
}

AwsSession& AwsSession::getInstance() {
  static AwsSession instance;
  return instance;
}

K initialize(K options) {
  if (options->t != XD && options->t != 101) {
    return krr("type");
  }

  try {
    return kb(AwsSession::getInstance().initialize(options));
  } catch (const std::exception& exc) {
    return krr(ss((S)exc.what()));
  }
}

const char * check_call(K options) {
  if (!AwsSession::getInstance().isInitialized()) { return "uninitialized"; }
  prune_retired();
  if (options == nullptr || (options->t != XD && options->t != 101)) { return "type"; }
  return nullptr;
}

K shutDown(K) {
  try {
    return kb(AwsSession::getInstance().shutDown());
  } catch (const std::exception & exc) {
    return krr(ss((S)exc.what()));
  }
}

}
