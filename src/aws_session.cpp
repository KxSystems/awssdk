#include <cctype>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>

#include <aws/core/platform/FileSystem.h>

#include "aws_session.h"
#include "s3_client.h"
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
  AwsSession::getInstance().shutDown();
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

bool AwsSession::shutDown() {
  if (!initialized) return false;
  // Clients hold SDK resources, so they have to go before ShutdownAPI.
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

K shutDown(K) {
  return kb(AwsSession::getInstance().shutDown());
}

}
