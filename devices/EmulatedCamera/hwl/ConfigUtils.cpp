/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "EmulatedCameraConfigUtils"

#include "ConfigUtils.h"

#include <android-base/file.h>
#include <android-base/strings.h>
#include <cutils/properties.h>
#include <hardware/camera_common.h>
#include <log/log.h>
#include <sys/stat.h>

#include "EmulatedSensor.h"
#include "utils/HWLUtils.h"
#include "vendor_tag_defs.h"

namespace android {

// Location of the camera configuration files.
constexpr std::string_view kCameraConfigBack = "emu_camera_back.json";
constexpr std::string_view kCameraConfigFront = "emu_camera_front.json";
constexpr std::string_view kCameraConfigExternal = "emu_camera_external.json";
constexpr std::string_view kCameraConfigDepth = "emu_camera_depth.json";
constexpr std::string_view kCameraConfigFiles[] = {
    kCameraConfigBack, kCameraConfigFront, kCameraConfigExternal,
    kCameraConfigDepth};

constexpr std::string_view kConfigurationFileDirVendor = "/vendor/etc/config/";
constexpr std::string_view kConfigurationFileDirApex =
    "/apex/com.google.emulated.camera.provider.hal/etc/config/";

status_t WaitForQemuSfFakeCameraPropertyAvailable() {
  // Camera service may start running before qemu-props sets
  // vendor.qemu.sf.fake_camera to any of the following four values:
  // "none,front,back,both"; so we need to wait.
  int num_attempts = 100;
  char prop[PROPERTY_VALUE_MAX];
  bool timeout = true;
  for (int i = 0; i < num_attempts; ++i) {
    if (property_get("vendor.qemu.sf.fake_camera", prop, nullptr) != 0) {
      timeout = false;
      break;
    }
    usleep(5000);
  }
  if (timeout) {
    ALOGE(
        "timeout (%dms) waiting for property vendor.qemu.sf.fake_camera to be "
        "set\n",
        5 * num_attempts);
    return BAD_VALUE;
  }
  return OK;
}

bool IsDigit(const std::string& value) {
  if (value.empty()) {
    return false;
  }

  for (const auto& c : value) {
    if (!std::isdigit(c) && (!std::ispunct(c))) {
      return false;
    }
  }

  return true;
}

template <typename T>
status_t GetEnumValue(uint32_t tag_id, const char* cstring, T* result /*out*/) {
  if ((result == nullptr) || (cstring == nullptr)) {
    return BAD_VALUE;
  }

  uint32_t enum_value;
  auto ret =
      camera_metadata_enum_value(tag_id, cstring, strlen(cstring), &enum_value);
  if (ret != OK) {
    ALOGE("%s: Failed to match tag id: 0x%x value: %s", __FUNCTION__, tag_id,
          cstring);
    return ret;
  }
  *result = enum_value;

  return OK;
}

status_t GetUInt8Value(const Json::Value& value, uint32_t tag_id,
                       uint8_t* result /*out*/) {
  if (result == nullptr) {
    return BAD_VALUE;
  }

  if (value.isString()) {
    errno = 0;
    if (IsDigit(value.asString())) {
      auto int_value = strtol(value.asCString(), nullptr, 10);
      if ((int_value >= 0) && (int_value <= UINT8_MAX) && (errno == 0)) {
        *result = int_value;
      } else {
        ALOGE("%s: Failed parsing tag id 0x%x", __func__, tag_id);
        return BAD_VALUE;
      }
    } else {
      return GetEnumValue(tag_id, value.asCString(), result);
    }
  } else {
    ALOGE(
        "%s: Unexpected json type: %d! All value types are expected to be "
        "strings!",
        __FUNCTION__, value.type());
    return BAD_VALUE;
  }

  return OK;
}

status_t GetInt32Value(const Json::Value& value, uint32_t tag_id,
                       int32_t* result /*out*/) {
  if (result == nullptr) {
    return BAD_VALUE;
  }

  if (value.isString()) {
    errno = 0;
    if (IsDigit(value.asString())) {
      auto int_value = strtol(value.asCString(), nullptr, 10);
      if ((int_value >= INT32_MIN) && (int_value <= INT32_MAX) && (errno == 0)) {
        *result = int_value;
      } else {
        ALOGE("%s: Failed parsing tag id 0x%x", __func__, tag_id);
        return BAD_VALUE;
      }
    } else {
      return GetEnumValue(tag_id, value.asCString(), result);
    }
  } else {
    ALOGE(
        "%s: Unexpected json type: %d! All value types are expected to be "
        "strings!",
        __FUNCTION__, value.type());
    return BAD_VALUE;
  }

  return OK;
}

status_t GetInt64Value(const Json::Value& value, uint32_t tag_id,
                       int64_t* result /*out*/) {
  if (result == nullptr) {
    return BAD_VALUE;
  }

  if (value.isString()) {
    errno = 0;
    auto int_value = strtoll(value.asCString(), nullptr, 10);
    if ((int_value >= INT64_MIN) && (int_value <= INT64_MAX) && (errno == 0)) {
      *result = int_value;
    } else {
      ALOGE("%s: Failed parsing tag id 0x%x", __func__, tag_id);
      return BAD_VALUE;
    }
  } else {
    ALOGE(
        "%s: Unexpected json type: %d! All value types are expected to be "
        "strings!",
        __FUNCTION__, value.type());
    return BAD_VALUE;
  }

  return OK;
}

status_t GetFloatValue(const Json::Value& value, uint32_t tag_id,
                       float* result /*out*/) {
  if (result == nullptr) {
    return BAD_VALUE;
  }

  if (value.isString()) {
    errno = 0;
    auto float_value = strtof(value.asCString(), nullptr);
    if (errno == 0) {
      *result = float_value;
    } else {
      ALOGE("%s: Failed parsing tag id 0x%x", __func__, tag_id);
      return BAD_VALUE;
    }
  } else {
    ALOGE(
        "%s: Unexpected json type: %d! All value types are expected to be "
        "strings!",
        __FUNCTION__, value.type());
    return BAD_VALUE;
  }

  return OK;
}

status_t GetDoubleValue(const Json::Value& value, uint32_t tag_id,
                        double* result /*out*/) {
  if (result == nullptr) {
    return BAD_VALUE;
  }

  if (value.isString()) {
    errno = 0;
    auto double_value = strtod(value.asCString(), nullptr);
    if (errno == 0) {
      *result = double_value;
    } else {
      ALOGE("%s: Failed parsing tag id 0x%x", __func__, tag_id);
      return BAD_VALUE;
    }
  } else {
    ALOGE(
        "%s: Unexpected json type: %d! All value types are expected to be "
        "strings!",
        __FUNCTION__, value.type());
    return BAD_VALUE;
  }

  return OK;
}

template <typename T>
void FilterVendorKeys(uint32_t tag_id, std::vector<T>* values) {
  if ((values == nullptr) || (values->empty())) {
    return;
  }

  switch (tag_id) {
    case ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS:
    case ANDROID_REQUEST_AVAILABLE_RESULT_KEYS:
    case ANDROID_REQUEST_AVAILABLE_SESSION_KEYS:
    case ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS: {
      auto it = values->begin();
      while (it != values->end()) {
        // Per spec. the tags we are handling here will be "int32_t".
        // In this case all vendor defined values will be negative.
        if (*it < 0) {
          it = values->erase(it);
        } else {
          it++;
        }
      }
    } break;
    default:
      // no-op
      break;
  }
}

template <typename T, typename func_type>
status_t InsertTag(const Json::Value& json_value, uint32_t tag_id,
                   func_type get_val_func, HalCameraMetadata* meta /*out*/) {
  if (meta == nullptr) {
    return BAD_VALUE;
  }

  std::vector<T> values;
  T result;
  status_t ret = -1;
  values.reserve(json_value.size());
  for (const auto& val : json_value) {
    ret = get_val_func(val, tag_id, &result);
    if (ret != OK) {
      break;
    }

    values.push_back(result);
  }

  if (ret == OK) {
    FilterVendorKeys(tag_id, &values);
    ret = meta->Set(tag_id, values.data(), values.size());
  }

  return ret;
}

status_t InsertRationalTag(const Json::Value& json_value, uint32_t tag_id,
                           HalCameraMetadata* meta /*out*/) {
  if (meta == nullptr) {
    return BAD_VALUE;
  }

  std::vector<camera_metadata_rational_t> values;
  status_t ret = OK;
  if (json_value.isArray() && ((json_value.size() % 2) == 0)) {
    values.reserve(json_value.size() / 2);
    auto it = json_value.begin();
    while (it != json_value.end()) {
      camera_metadata_rational_t result;
      ret = GetInt32Value((*it), tag_id, &result.numerator);
      it++;
      ret |= GetInt32Value((*it), tag_id, &result.denominator);
      it++;
      if (ret != OK) {
        break;
      }
      values.push_back(result);
    }
  } else {
    ALOGE("%s: json type: %d doesn't match with rational tag type",
          __FUNCTION__, json_value.type());
    return BAD_VALUE;
  }

  if (ret == OK) {
    ret = meta->Set(tag_id, values.data(), values.size());
  }

  return ret;
}

status_t GetTagFromName(const char* name, uint32_t* tag) {
  if (name == nullptr || tag == nullptr) {
    return BAD_VALUE;
  }

  size_t name_length = strlen(name);
  // First, find the section by the longest string match
  const char* section = NULL;
  size_t section_index = 0;
  size_t section_length = 0;
  for (size_t i = 0; i < ANDROID_SECTION_COUNT; ++i) {
    const char* str = camera_metadata_section_names[i];

    ALOGV("%s: Trying to match against section '%s'", __FUNCTION__, str);

    if (strstr(name, str) == name) {  // name begins with the section name
      size_t str_length = strlen(str);

      ALOGV("%s: Name begins with section name", __FUNCTION__);

      // section name is the longest we've found so far
      if (section == NULL || section_length < str_length) {
        section = str;
        section_index = i;
        section_length = str_length;

        ALOGV("%s: Found new best section (%s)", __FUNCTION__, section);
      }
    }
  }

  if (section == NULL) {
    return NAME_NOT_FOUND;
  } else {
    ALOGV("%s: Found matched section '%s' (%zu)", __FUNCTION__, section,
          section_index);
  }

  // Get the tag name component of the name
  const char* name_tag_name = name + section_length + 1;  // x.y.z -> z
  if (section_length + 1 >= name_length) {
    return BAD_VALUE;
  }

  // Match rest of name against the tag names in that section only
  uint32_t candidate_tag = 0;
  // Match built-in tags (typically android.*)
  uint32_t tag_begin, tag_end;  // [tag_begin, tag_end)
  tag_begin = camera_metadata_section_bounds[section_index][0];
  tag_end = camera_metadata_section_bounds[section_index][1];

  for (candidate_tag = tag_begin; candidate_tag < tag_end; ++candidate_tag) {
    const char* tag_name = get_camera_metadata_tag_name(candidate_tag);

    if (strcmp(name_tag_name, tag_name) == 0) {
      ALOGV("%s: Found matched tag '%s' (%d)", __FUNCTION__, tag_name,
            candidate_tag);
      break;
    }
  }

  if (candidate_tag == tag_end) {
    return NAME_NOT_FOUND;
  }

  *tag = candidate_tag;
  return OK;
}

status_t ParseCharacteristics(const Json::Value& value,
                              CameraConfiguration* config) {
  if (!value.isObject()) {
    ALOGE("%s: Configuration root is not an object", __FUNCTION__);
    return BAD_VALUE;
  }

  auto static_meta = HalCameraMetadata::Create(1, 10);
  auto members = value.getMemberNames();
  for (const auto& member : members) {
    uint32_t tag_id;
    auto stat = GetTagFromName(member.c_str(), &tag_id);
    if (stat != OK) {
      ALOGE("%s: tag %s not supported, skipping!", __func__, member.c_str());
      continue;
    }

    auto tag_type = get_camera_metadata_tag_type(tag_id);
    auto tag_value = value[member.c_str()];
    switch (tag_type) {
      case TYPE_BYTE:
        InsertTag<uint8_t>(tag_value, tag_id, GetUInt8Value, static_meta.get());
        break;
      case TYPE_INT32:
        InsertTag<int32_t>(tag_value, tag_id, GetInt32Value, static_meta.get());
        break;
      case TYPE_INT64:
        InsertTag<int64_t>(tag_value, tag_id, GetInt64Value, static_meta.get());
        break;
      case TYPE_FLOAT:
        InsertTag<float>(tag_value, tag_id, GetFloatValue, static_meta.get());
        break;
      case TYPE_DOUBLE:
        InsertTag<double>(tag_value, tag_id, GetDoubleValue, static_meta.get());
        break;
      case TYPE_RATIONAL:
        InsertRationalTag(tag_value, tag_id, static_meta.get());
        break;
      default:
        ALOGE("%s: Unsupported tag type: %d!", __FUNCTION__, tag_type);
    }
  }

  SensorCharacteristics sensor_characteristics;
  status_t ret =
      GetSensorCharacteristics(static_meta.get(), &sensor_characteristics);
  if (ret != OK) {
    ALOGE("%s: Unable to extract sensor characteristics!", __FUNCTION__);
    return ret;
  }

  if (!EmulatedSensor::AreCharacteristicsSupported(sensor_characteristics)) {
    ALOGE("%s: Sensor characteristics not supported!", __FUNCTION__);
    return BAD_VALUE;
  }

  // Although we don't support HdrPlus, this data is still required by HWL
  int32_t payload_frames = 0;
  static_meta->Set(google_camera_hal::kHdrplusPayloadFrames, &payload_frames, 1);

  config->characteristics = std::move(static_meta);

  return OK;
}

status_t GetCameraConfigurations(std::vector<CameraConfiguration>* configs) {
  if (configs == nullptr) {
    return BAD_VALUE;
  }
  configs->clear();

  std::string config_content;
  uint32_t logical_id_counter = 0;
  uint32_t physical_id_counter = ARRAY_SIZE(kCameraConfigFiles);

  std::vector<std::string> config_file_locations;
  std::string config_dir = "";
  struct stat st;
  if (stat(kConfigurationFileDirApex.data(), &st) == 0) {
    config_dir += kConfigurationFileDirApex.data();
  } else {
    config_dir += kConfigurationFileDirVendor.data();
  }
  char prop[PROPERTY_VALUE_MAX];
  if (!property_get_bool("ro.boot.qemu", false)) {
    // Cuttlefish
    property_get("ro.vendor.camera.config", prop, nullptr);
    if (strcmp(prop, "external") == 0) {
      config_file_locations.emplace_back(config_dir +
                                         std::string(kCameraConfigExternal));
      logical_id_counter = 1;
    } else {
      // Default phone layout.
      config_file_locations.emplace_back(config_dir +
                                         std::string(kCameraConfigBack));
      config_file_locations.emplace_back(config_dir +
                                         std::string(kCameraConfigFront));
      config_file_locations.emplace_back(config_dir +
                                         std::string(kCameraConfigDepth));
    }
  } else {
    // Android Studio Emulator
    if (!property_get_bool("ro.boot.qemu.legacy_fake_camera", false)) {
      if (WaitForQemuSfFakeCameraPropertyAvailable() == OK) {
        property_get("vendor.qemu.sf.fake_camera", prop, nullptr);
        if (strcmp(prop, "both") == 0) {
          config_file_locations.emplace_back(config_dir +
                                             std::string(kCameraConfigBack));
          config_file_locations.emplace_back(config_dir +
                                             std::string(kCameraConfigFront));
        } else if (strcmp(prop, "front") == 0) {
          config_file_locations.emplace_back(config_dir +
                                             std::string(kCameraConfigFront));
          logical_id_counter = 1;
        } else if (strcmp(prop, "back") == 0) {
          config_file_locations.emplace_back(config_dir +
                                             std::string(kCameraConfigBack));
          logical_id_counter = 1;
        }
      }
    }
  }

  for (const auto& config_path : config_file_locations) {
    if (!android::base::ReadFileToString(config_path, &config_content)) {
      ALOGW("%s: Could not open configuration file: %s", __FUNCTION__,
            config_path.c_str());
      return BAD_VALUE;
    }

    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> config_reader(builder.newCharReader());
    Json::Value root;
    std::string error_message;
    if (!config_reader->parse(&*config_content.begin(), &*config_content.end(),
                              &root, &error_message)) {
      ALOGE("Could not parse configuration file: %s", error_message.c_str());
      return BAD_VALUE;
    }

    CameraConfiguration cam_config;
    cam_config.id = logical_id_counter;

    if (root.isArray()) {
      auto device_iter = root.begin();
      if (ParseCharacteristics(*device_iter, &cam_config) != OK) {
        return BAD_VALUE;
      }
      device_iter++;

      if (root.size() >= 3) {
        while (device_iter != root.end()) {
          CameraConfiguration physical_config;
          if (ParseCharacteristics(*device_iter, &physical_config) == OK) {
            cam_config.physical_camera_characteristics[physical_id_counter++] =
                std::move(physical_config.characteristics);
          }
          device_iter++;
        }
      }
    } else {
      if (ParseCharacteristics(root, &cam_config) != OK) {
        return BAD_VALUE;
      }
    }
    configs->push_back(std::move(cam_config));
    logical_id_counter++;
  }

  return OK;
}

}  // namespace android
