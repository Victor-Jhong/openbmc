// Copyright 2021-present Facebook. All Rights Reserved.
#include "ModbusDevice.h"
#include <iomanip>
#include <sstream>
#include "Log.h"

using nlohmann::json;

namespace rackmon {

ModbusDevice::ModbusDevice(
    Modbus& interface,
    uint8_t deviceAddress,
    const RegisterMap& registerMap,
    int numCommandRetries)
    : interface_(interface),
      numCommandRetries_(numCommandRetries),
      baudConfig_(registerMap.baudConfig) {
  info_.deviceAddress = deviceAddress;
  info_.preferredBaudrate = registerMap.preferredBaudrate;
  info_.defaultBaudrate = registerMap.defaultBaudrate;
  info_.baudrate = info_.defaultBaudrate;
  info_.deviceType = registerMap.name;
  info_.parity = registerMap.parity;

  for (auto& it : registerMap.registerDescriptors) {
    info_.registerList.emplace_back(it.second);
  }

  for (const auto& sp : registerMap.specialHandlers) {
    ModbusSpecialHandler hdl(deviceAddress);
    hdl.SpecialHandlerInfo::operator=(sp);
    specialHandlers_.push_back(hdl);
  }
}

void ModbusDevice::handleCommandFailure(std::exception& baseException) {
  if (TimeoutException * ex;
      (ex = dynamic_cast<TimeoutException*>(&baseException)) != nullptr) {
    info_.timeouts++;
  } else if (CRCError * ex;
             (ex = dynamic_cast<CRCError*>(&baseException)) != nullptr) {
    info_.crcErrors++;
  } else if (ModbusError * ex;
             (ex = dynamic_cast<ModbusError*>(&baseException)) != nullptr) {
    // ModbusErrors can happen in normal operation. Do not let
    // it increment numConsecutiveFailures since it should not
    // account as a signal of a device being dormant.
    info_.deviceErrors++;
    return;
  } else if (std::system_error * ex; (ex = dynamic_cast<std::system_error*>(
                                          &baseException)) != nullptr) {
    info_.miscErrors++;
    logError << ex->what() << std::endl;
  } else {
    info_.miscErrors++;
    logError << baseException.what() << std::endl;
  }
  if ((++info_.numConsecutiveFailures) >= kMaxConsecutiveFailures) {
    // If we are in exclusive mode let it continue to
    // fail. We will mark it as dormant when we exit
    // exclusive mode.
    if (!exclusiveMode_) {
      info_.mode = ModbusDeviceMode::DORMANT;
    }
  }
}

void ModbusDevice::command(Msg& req, Msg& resp, ModbusTime timeout) {
  size_t reqLen = req.len;
  size_t respLen = resp.len;
  // Try executing the command, if errors, catch the error
  // to maintain stats on types of errors and re-throw (on
  // the last retry) in case the user wants to handle them
  // in a special way.
  int numRetries = exclusiveMode_ ? 1 : numCommandRetries_;
  for (int retries = 0; retries < numRetries; retries++) {
    try {
      interface_.command(req, resp, info_.baudrate, timeout, info_.parity);
      info_.numConsecutiveFailures = 0;
      info_.lastActive = std::time(nullptr);
      break;
    } catch (std::exception& ex) {
      handleCommandFailure(ex);
      if (retries == (numRetries - 1)) {
        throw;
      }
      req.len = reqLen;
      resp.len = respLen;
    }
  }
}

void ModbusDevice::readHoldingRegisters(
    uint16_t registerOffset,
    std::vector<uint16_t>& regs,
    ModbusTime timeout) {
  ReadHoldingRegistersReq req(info_.deviceAddress, registerOffset, regs.size());
  ReadHoldingRegistersResp resp(info_.deviceAddress, regs);
  command(req, resp, timeout);
}

void ModbusDevice::writeSingleRegister(
    uint16_t registerOffset,
    uint16_t value,
    ModbusTime timeout) {
  WriteSingleRegisterReq req(info_.deviceAddress, registerOffset, value);
  WriteSingleRegisterResp resp(info_.deviceAddress, registerOffset);
  command(req, resp, timeout);
}

void ModbusDevice::writeMultipleRegisters(
    uint16_t registerOffset,
    std::vector<uint16_t>& value,
    ModbusTime timeout) {
  WriteMultipleRegistersReq req(info_.deviceAddress, registerOffset);
  for (uint16_t val : value) {
    req << val;
  }
  WriteMultipleRegistersResp resp(
      info_.deviceAddress, registerOffset, value.size());
  command(req, resp, timeout);
}

void ModbusDevice::readFileRecord(
    std::vector<FileRecord>& records,
    ModbusTime timeout) {
  ReadFileRecordReq req(info_.deviceAddress, records);
  ReadFileRecordResp resp(info_.deviceAddress, records);
  command(req, resp, timeout);
}

void ModbusDevice::setBaudrate(uint32_t baud) {
  // Return early if earlier setBaud failed, or
  // we dont have configuration or if we already
  // are at the requested baudrate.
  if (!setBaudEnabled_ || !baudConfig_.isSet || baud == info_.baudrate) {
    return;
  }
  try {
    writeSingleRegister(baudConfig_.reg, baudConfig_.baudValueMap.at(baud));
    info_.baudrate = baud;
  } catch (std::exception&) {
    setBaudEnabled_ = false;
    logError << "Failed to set baudrate to " << baud << std::endl;
  }
}

bool ModbusDevice::reloadRegister(RegisterStore& registerStore) {
  if (!registerStore.isEnabled()) {
    return false;
  }
  uint16_t registerOffset = registerStore.regAddr();
  try {
    std::vector<uint16_t>& value = registerStore.beginReloadRegister();
    readHoldingRegisters(registerOffset, value);
    registerStore.endReloadRegister();
  } catch (ModbusError& e) {
    logInfo << "DEV:0x" << std::hex << int(info_.deviceAddress) << " ReadReg 0x"
            << std::hex << registerOffset << ' ' << registerStore.name()
            << " caught: " << e.what() << std::endl;
    if (e.errorCode == ModbusErrorCode::ILLEGAL_DATA_ADDRESS) {
      logInfo << "DEV:0x" << std::hex << int(info_.deviceAddress)
              << " ReadReg 0x" << std::hex << registerOffset << ' '
              << registerStore.name()
              << " unsupported. Disabled from monitoring" << std::endl;
      registerStore.disable();
    } else {
      logInfo << "DEV:0x" << std::hex << int(info_.deviceAddress)
              << " ReadReg 0x" << std::hex << registerOffset << ' '
              << registerStore.name() << " caught: " << e.what() << std::endl;
    }
  } catch (std::exception& e) {
    logInfo << "DEV:0x" << std::hex << int(info_.deviceAddress) << " ReadReg 0x"
            << std::hex << registerOffset << ' ' << registerStore.name()
            << " caught: " << e.what() << std::endl;
  }
  return true;
}

void ModbusDevice::reloadRegisters() {
  setPreferredBaudrate();
  // If the number of consecutive failures has exceeded
  // a threshold, mark the device as dormant.
  for (auto& specialHandler : specialHandlers_) {
    // Break early, if we are entering exclusive mode
    if (exclusiveMode_) {
      break;
    }
    specialHandler.handle(*this);
  }
  for (auto& registerStore : info_.registerList) {
    // Break early, if we are entering exclusive mode
    if (exclusiveMode_) {
      break;
    }
    if (reloadRegister(registerStore)) {
      // Release thread to allow for higher priority tasks to execute.
      std::this_thread::yield();
    }
  }
}

void ModbusDevice::setActive() {
  // Enable any disabled registers. Assumption is
  // that the device might be unplugged and replugged
  // with a newer version. Thus, prepare for the
  // newer register set.
  for (auto& registerStore : info_.registerList) {
    registerStore.enable();
  }
  // Clear the num failures so we consider it active.
  info_.numConsecutiveFailures = 0;
  info_.mode = ModbusDeviceMode::ACTIVE;
}

ModbusDeviceRawData ModbusDevice::getRawData() {
  // Makes a deep copy.
  return info_;
}

ModbusDeviceInfo ModbusDevice::getInfo() {
  return info_;
}

ModbusDeviceValueData ModbusDevice::getValueData(
    const ModbusRegisterFilter& filter,
    bool latestValueOnly) const {
  ModbusDeviceValueData data;
  data.ModbusDeviceInfo::operator=(info_);
  auto shouldPickRegister = [&filter](const RegisterStore& reg) {
    return !filter || filter.contains(reg.regAddr()) ||
        filter.contains(reg.name());
  };
  for (const auto& reg : info_.registerList) {
    if (shouldPickRegister(reg)) {
      if (latestValueOnly) {
        data.registerList.emplace_back(reg.regAddr(), reg.name());
        data.registerList.back().history.emplace_back(reg.back());
      } else {
        data.registerList.emplace_back(reg);
      }
    }
  }
  return data;
}

static std::string commandOutput(const std::string& shell) {
  std::array<char, 128> buffer;
  std::string result;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(
      popen(shell.c_str(), "r"), pclose);
  if (!pipe) {
    throw std::runtime_error("popen() failed!");
  }
  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }
  return result;
}

void ModbusSpecialHandler::handle(ModbusDevice& dev) {
  // Check if it is time to handle.
  if (!canHandle()) {
    return;
  }
  std::string strValue{};
  WriteMultipleRegistersReq req(deviceAddress_, reg);
  if (info.shell) {
    // The command is from the JSON configuration.
    // TODO, we currently only have need to set the
    // current UNIX time. If we want to avoid shell,
    // we might need a different way to generalize
    // this.
    strValue = commandOutput(info.shell.value());
  } else if (info.value) {
    strValue = info.value.value();
  } else {
    logError << "NULL action ignored" << std::endl;
    return;
  }
  if (info.interpret == RegisterValueType::INTEGER) {
    int32_t ival = std::stoi(strValue);
    if (len == 1) {
      req << uint16_t(ival);
    } else if (len == 2) {
      req << uint32_t(ival);
    } else {
      logError << "Value truncated to 32bits" << std::endl;
      req << uint32_t(ival);
    }
  } else if (info.interpret == RegisterValueType::STRING) {
    for (char c : strValue) {
      req << uint8_t(c);
    }
  }
  WriteMultipleRegistersResp resp(deviceAddress_, reg, len);
  try {
    dev.command(req, resp);
  } catch (ModbusError& e) {
    if (e.errorCode == ModbusErrorCode::ILLEGAL_DATA_ADDRESS ||
        e.errorCode == ModbusErrorCode::ILLEGAL_FUNCTION) {
      logInfo << "DEV:0x" << std::hex << +deviceAddress_
              << " Special Handler at 0x" << std::hex << +reg << ' '
              << " unsupported. Disabled from future updates" << std::endl;
      period = -1;
    } else {
      logInfo << "DEV:0x" << std::hex << +deviceAddress_
              << " Special Handler at 0x" << std::hex << +reg << ' '
              << " caught: " << e.what() << std::endl;
    }
  } catch (std::exception& e) {
    logInfo << "DEV:0x" << std::hex << +deviceAddress_
            << " Special Handler at 0x" << std::hex << +reg << ' '
            << " caught: " << e.what() << std::endl;
  }
  lastHandleTime_ = getTime();
  handled_ = true;
}

NLOHMANN_JSON_SERIALIZE_ENUM(
    ModbusDeviceMode,
    {{ModbusDeviceMode::ACTIVE, "ACTIVE"},
     {ModbusDeviceMode::DORMANT, "DORMANT"}})

void to_json(json& j, const ModbusDeviceInfo& m) {
  j["devAddress"] = m.deviceAddress;
  j["deviceType"] = m.deviceType;
  j["crcErrors"] = m.crcErrors;
  j["timeouts"] = m.timeouts;
  j["miscErrors"] = m.miscErrors;
  j["baudrate"] = m.baudrate;
  j["mode"] = m.mode;
}

// Legacy JSON format.
void to_json(json& j, const ModbusDeviceRawData& m) {
  j["addr"] = m.deviceAddress;
  j["crc_fails"] = m.crcErrors;
  j["timeouts"] = m.timeouts;
  j["misc_fails"] = m.miscErrors;
  j["mode"] = m.mode;
  j["baudrate"] = m.baudrate;
  j["deviceType"] = m.deviceType;
  j["now"] = std::time(nullptr);
  j["ranges"] = m.registerList;
}

// v2.0 JSON Format.
void to_json(json& j, const ModbusDeviceValueData& m) {
  const ModbusDeviceInfo& devInfo = m;
  j["devInfo"] = devInfo;
  j["regList"] = m.registerList;
}

} // namespace rackmon
