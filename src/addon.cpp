#include "cluster.h"
#include "io_context.h"
#include "rbd.h"
#include "events.h"

#include <dlfcn.h>

#include <limits>
#include <sstream>

namespace nl {

Napi::Error ceph_error(Napi::Env env, int code, const std::string& operation,
                       const std::string& resource) {
  const int positive = code < 0 ? -code : code;
  std::ostringstream message;
  message << operation;
  if (!resource.empty()) message << " (" << resource << ")";
  message << ": " << std::strerror(positive);
  Napi::Error error = Napi::Error::New(env, message.str());
  error.Set("name", "CephError");
  error.Set("code", Napi::Number::New(env, code));
  error.Set("errno", Napi::Number::New(env, positive));
  error.Set("operation", operation);
  if (!resource.empty()) error.Set("resource", resource);
  return error;
}

void throw_ceph(Napi::Env env, int code, const std::string& operation,
                const std::string& resource) {
  ceph_error(env, code, operation, resource).ThrowAsJavaScriptException();
}

uint64_t uint64_arg(const Napi::Value& value, const char* name) {
  if (value.IsBigInt()) {
    bool lossless = false;
    uint64_t result = value.As<Napi::BigInt>().Uint64Value(&lossless);
    if (!lossless) throw Napi::RangeError::New(value.Env(), std::string(name) + " is outside uint64 range");
    return result;
  }
  if (value.IsNumber()) {
    const double number = value.As<Napi::Number>().DoubleValue();
    if (number < 0 || number > 9007199254740991.0 || number != static_cast<uint64_t>(number))
      throw Napi::RangeError::New(value.Env(), std::string(name) + " must be a safe non-negative integer or bigint");
    return static_cast<uint64_t>(number);
  }
  throw Napi::TypeError::New(value.Env(), std::string(name) + " must be a bigint or number");
}

int64_t int64_arg(const Napi::Value& value, const char* name) {
  if (value.IsBigInt()) {
    bool lossless = false;
    int64_t result = value.As<Napi::BigInt>().Int64Value(&lossless);
    if (!lossless) throw Napi::RangeError::New(value.Env(), std::string(name) + " is outside int64 range");
    return result;
  }
  if (value.IsNumber()) {
    const double number = value.As<Napi::Number>().DoubleValue();
    if (number < -9007199254740991.0 || number > 9007199254740991.0 || number != static_cast<int64_t>(number))
      throw Napi::RangeError::New(value.Env(), std::string(name) + " must be a safe integer or bigint");
    return static_cast<int64_t>(number);
  }
  throw Napi::TypeError::New(value.Env(), std::string(name) + " must be a bigint or number");
}

std::string string_arg(const Napi::CallbackInfo& info, size_t index, const char* name) {
  if (info.Length() <= index || !info[index].IsString())
    throw Napi::TypeError::New(info.Env(), std::string(name) + " must be a string");
  return info[index].As<Napi::String>().Utf8Value();
}

Napi::Object version_object(Napi::Env env, int major, int minor, int extra) {
  auto value = Napi::Object::New(env);
  value.Set("major", major);
  value.Set("minor", minor);
  value.Set("extra", extra);
  value.Set("string", std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(extra));
  return value;
}

static Napi::Object Versions(const Napi::CallbackInfo& info) {
  int rados_major = 0, rados_minor = 0, rados_extra = 0;
  int rbd_major = 0, rbd_minor = 0, rbd_extra = 0;
  rados_version(&rados_major, &rados_minor, &rados_extra);
  rbd_version(&rbd_major, &rbd_minor, &rbd_extra);
  auto result = Napi::Object::New(info.Env());
  result.Set("rados", version_object(info.Env(), rados_major, rados_minor, rados_extra));
  result.Set("rbd", version_object(info.Env(), rbd_major, rbd_minor, rbd_extra));
  result.Set("napi", NAPI_VERSION);
  return result;
}

static Napi::Object Capabilities(const Napi::CallbackInfo& info) {
  static const std::pair<const char*, const char*> symbols[] = {
      {"radosWatch", "rados_watch3"},
      {"radosMonitorLog", "rados_monitor_log2"},
      {"radosService", "rados_service_register"},
      {"radosBlocklist", "rados_blocklist_add"},
      {"rbdNamespace", "rbd_namespace_create"},
      {"rbdTrash", "rbd_trash_move"},
      {"rbdMigration", "rbd_migration_prepare"},
      {"rbdEncryption", "rbd_encryption_format"},
      {"rbdGroups", "rbd_group_create"},
      {"rbdMirroring", "rbd_mirror_mode_set"},
      {"rbdUpdateWatch", "rbd_update_watch"},
      {"rbdQuiesceWatch", "rbd_quiesce_watch"},
  };
  auto result = Napi::Object::New(info.Env());
  for (const auto& [name, symbol] : symbols)
    result.Set(name, Napi::Boolean::New(info.Env(), dlsym(RTLD_DEFAULT, symbol) != nullptr));
  return result;
}

static Napi::Object Init(Napi::Env env, Napi::Object exports) {
  IoContextWrap::Init(env, exports);
  ObjectListWrap::Init(env, exports);
  ObjectWatchWrap::Init(env, exports);
  MonitorLogWrap::Init(env, exports);
  ImageUpdateWatchWrap::Init(env, exports);
  RbdPoolWrap::Init(env, exports);
  RbdImageWrap::Init(env, exports);
  ClusterWrap::Init(env, exports);
  exports.Set("versions", Napi::Function::New(env, Versions));
  exports.Set("capabilities", Napi::Function::New(env, Capabilities));
  return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, Init)

}  // namespace nl
