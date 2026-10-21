#include <cctype>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>

#include <aws/core/platform/FileSystem.h>
#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/logging/LogSystemInterface.h>

#include "aws_session.h"
#include "s3_client.h"
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

// The SDK is left initialized at exit, as awss3kdb leaves it: its threads may
// still be running, and ShutdownAPI under them would be worse than not at all.
// Only its log is flushed, which would otherwise lose whatever is buffered.
static void flush_log_at_exit() {
  if (!AwsSession::getInstance().isInitialized()) return;
  if (auto log = Aws::Utils::Logging::GetLogSystem()) { log->Flush(); }
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
    std::atexit(&flush_log_at_exit);
    atexit_registered = true;
  }
  return true;
}

// As awss3kdb's shutdownSDK: releases every client and transfer manager, then
// shuts the SDK down. Refused while an async transfer is running, whose SDK
// threads would otherwise be working on after ShutdownAPI.
bool AwsSession::shutDown() {
  if (!initialized) return false;
  if (async_transfers_in_flight()) { throw std::runtime_error("busy"); }
  // Transfer managers first: each one holds a client.
  destroy_all_transfer_managers();
  destroy_all_clients();
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
