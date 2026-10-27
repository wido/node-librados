#include "io_context.h"

#include "rbd.h"
#include "events.h"

#include <ctime>
#include <map>

namespace nl {

Napi::FunctionReference IoContextWrap::constructor;

namespace {
using Bytes = std::vector<uint8_t>;
using ByteMap = std::vector<std::pair<std::string, Bytes>>;
struct ObjectStat { uint64_t size{}; time_t modified{}; };
struct ObjectEntry { std::string name; std::string locator; std::string nspace; };
struct OmapResult { ByteMap entries; bool more{}; };

Bytes buffer_arg(const Napi::CallbackInfo& info, size_t index, const char* name) {
  if (info.Length() <= index || !info[index].IsBuffer())
    throw Napi::TypeError::New(info.Env(), std::string(name) + " must be a Buffer");
  auto value = info[index].As<Napi::Buffer<uint8_t>>();
  return {value.Data(), value.Data() + value.Length()};
}

Napi::Object byte_map_to_js(Napi::Env env, const ByteMap& values) {
  auto out = Napi::Object::New(env);
  for (const auto& [key, value] : values)
    out.Set(key, Napi::Buffer<uint8_t>::Copy(env, value.data(), value.size()));
  return out;
}
}

void IoContextWrap::Init(Napi::Env env, Napi::Object exports) {
  auto function = DefineClass(env, "NativeIoContext", {
      InstanceMethod("close", &IoContextWrap::Close),
      InstanceAccessor("closed", &IoContextWrap::IsClosed, nullptr),
      InstanceAccessor("poolName", &IoContextWrap::PoolName, nullptr),
      InstanceAccessor("poolId", &IoContextWrap::PoolId, nullptr),
      InstanceMethod("setNamespace", &IoContextWrap::SetNamespace),
      InstanceMethod("setLocatorKey", &IoContextWrap::SetLocatorKey),
      InstanceMethod("setReadSnapshot", &IoContextWrap::SetReadSnapshot),
      InstanceAccessor("lastVersion", &IoContextWrap::LastVersion, nullptr),
      InstanceMethod("requiredAlignment", &IoContextWrap::Alignment),
      InstanceMethod("requiresAlignment", &IoContextWrap::RequiresAlignment),
      InstanceMethod("write", &IoContextWrap::Write),
      InstanceMethod("writeFull", &IoContextWrap::WriteFull),
      InstanceMethod("append", &IoContextWrap::Append),
      InstanceMethod("read", &IoContextWrap::Read),
      InstanceMethod("remove", &IoContextWrap::Remove),
      InstanceMethod("truncate", &IoContextWrap::Truncate),
      InstanceMethod("stat", &IoContextWrap::Stat),
      InstanceMethod("setXattr", &IoContextWrap::SetXattr),
      InstanceMethod("getXattr", &IoContextWrap::GetXattr),
      InstanceMethod("removeXattr", &IoContextWrap::RemoveXattr),
      InstanceMethod("getXattrs", &IoContextWrap::GetXattrs),
      InstanceMethod("getOmap", &IoContextWrap::GetOmap),
      InstanceMethod("setOmap", &IoContextWrap::SetOmap),
      InstanceMethod("removeOmapKeys", &IoContextWrap::RemoveOmapKeys),
      InstanceMethod("listObjects", &IoContextWrap::ListObjects),
      InstanceMethod("notify", &IoContextWrap::Notify),
      InstanceMethod("flush", &IoContextWrap::Flush),
      InstanceMethod("lockExclusive", &IoContextWrap::LockExclusive),
      InstanceMethod("lockShared", &IoContextWrap::LockShared),
      InstanceMethod("unlock", &IoContextWrap::Unlock),
      InstanceMethod("breakLock", &IoContextWrap::BreakLock),
      InstanceMethod("rbd", &IoContextWrap::Rbd),
      InstanceMethod("watch", &IoContextWrap::Watch),
  });
  constructor = Napi::Persistent(function); constructor.SuppressDestruct();
  exports.Set("NativeIoContext", function);
}

Napi::Object IoContextWrap::NewInstance(Napi::Env env, std::shared_ptr<IoState> state,
                                        std::shared_ptr<ClusterState> cluster,
                                        std::string pool) {
  using Init = std::tuple<std::shared_ptr<IoState>, std::shared_ptr<ClusterState>, std::string>;
  auto* init = new Init(std::move(state), std::move(cluster), std::move(pool));
  return constructor.New({Napi::External<Init>::New(env, init)});
}

IoContextWrap::IoContextWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<IoContextWrap>(info) {
  using Init = std::tuple<std::shared_ptr<IoState>, std::shared_ptr<ClusterState>, std::string>;
  if (!info.Length() || !info[0].IsExternal()) throw Napi::TypeError::New(info.Env(), "IoContext cannot be constructed directly");
  auto* init = info[0].As<Napi::External<Init>>().Data();
  state_ = std::move(std::get<0>(*init)); cluster_ = std::move(std::get<1>(*init)); pool_ = std::move(std::get<2>(*init)); delete init;
}

Napi::Value IoContextWrap::Close(const Napi::CallbackInfo& info) {
  auto state = state_; return async_void(info.Env(), "rados_ioctx_destroy", pool_, [state] { return state->close_checked(); });
}
Napi::Value IoContextWrap::IsClosed(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), state_->closed()); }
Napi::Value IoContextWrap::PoolName(const Napi::CallbackInfo& info) { return Napi::String::New(info.Env(), pool_); }
Napi::Value IoContextWrap::PoolId(const Napi::CallbackInfo& info) {
  int64_t id = state_->use([](rados_ioctx_t h) { return rados_ioctx_get_id(h); }); return Napi::BigInt::New(info.Env(), id);
}
Napi::Value IoContextWrap::SetNamespace(const Napi::CallbackInfo& info) {
  auto value = string_arg(info, 0, "namespace"); state_->use([&](rados_ioctx_t h) { rados_ioctx_set_namespace(h, value.c_str()); return 0; }); return info.Env().Undefined();
}
Napi::Value IoContextWrap::SetLocatorKey(const Napi::CallbackInfo& info) {
  std::optional<std::string> value; if (info.Length() && !info[0].IsNull()) value = string_arg(info, 0, "key");
  state_->use([&](rados_ioctx_t h) { rados_ioctx_locator_set_key(h, value ? value->c_str() : nullptr); return 0; }); return info.Env().Undefined();
}
Napi::Value IoContextWrap::SetReadSnapshot(const Napi::CallbackInfo& info) {
  uint64_t snapshot = uint64_arg(info[0], "snapshot"); state_->use([&](rados_ioctx_t h) { rados_ioctx_snap_set_read(h, snapshot); return 0; }); return info.Env().Undefined();
}
Napi::Value IoContextWrap::LastVersion(const Napi::CallbackInfo& info) { return Napi::BigInt::New(info.Env(), state_->use([](rados_ioctx_t h) { return rados_get_last_version(h); })); }

Napi::Value IoContextWrap::Alignment(const Napi::CallbackInfo& info) {
  auto state = state_; return async_value<uint64_t>(info.Env(), "rados_ioctx_pool_required_alignment2", pool_, [state](uint64_t& out) { return state->use([&](rados_ioctx_t h) { return rados_ioctx_pool_required_alignment2(h, &out); }); }, [](Napi::Env env, uint64_t& out) { return Napi::BigInt::New(env, out); });
}
Napi::Value IoContextWrap::RequiresAlignment(const Napi::CallbackInfo& info) {
  auto state = state_; return async_value<int>(info.Env(), "rados_ioctx_pool_requires_alignment2", pool_, [state](int& out) { return state->use([&](rados_ioctx_t h) { return rados_ioctx_pool_requires_alignment2(h, &out); }); }, [](Napi::Env env, int& out) { return Napi::Boolean::New(env, out != 0); });
}

Napi::Value IoContextWrap::Write(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"); BufferView data(info, 1, "data"); uint64_t offset = uint64_arg(info[2], "offset"); auto state = state_;
  return async_void(info.Env(), "rados_write", oid, [state, oid, data, offset] { return state->use([&](rados_ioctx_t h) { return rados_write(h, oid.c_str(), data.data(), data.size(), offset); }); });
}
Napi::Value IoContextWrap::WriteFull(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"); BufferView data(info, 1, "data"); auto state = state_;
  return async_void(info.Env(), "rados_write_full", oid, [state, oid, data] { return state->use([&](rados_ioctx_t h) { return rados_write_full(h, oid.c_str(), data.data(), data.size()); }); });
}
Napi::Value IoContextWrap::Append(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"); BufferView data(info, 1, "data"); auto state = state_;
  return async_void(info.Env(), "rados_append", oid, [state, oid, data] { return state->use([&](rados_ioctx_t h) { return rados_append(h, oid.c_str(), data.data(), data.size()); }); });
}
Napi::Value IoContextWrap::Read(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"); size_t length = static_cast<size_t>(uint64_arg(info[1], "length")); uint64_t offset = uint64_arg(info[2], "offset"); auto state = state_;
  return async_value<ReadBytes>(info.Env(), "rados_read", oid, [state, oid, length, offset](ReadBytes& out) { char* buffer = out.allocate(length); int rc = state->use([&](rados_ioctx_t h) { return rados_read(h, oid.c_str(), buffer, length, offset); }); if (rc >= 0) out.size = static_cast<size_t>(rc); return rc < 0 ? rc : 0; }, [](Napi::Env env, ReadBytes& out) { return out.to_buffer(env); });
}
Napi::Value IoContextWrap::Remove(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"); auto state = state_; return async_void(info.Env(), "rados_remove", oid, [state, oid] { return state->use([&](rados_ioctx_t h) { return rados_remove(h, oid.c_str()); }); });
}
Napi::Value IoContextWrap::Truncate(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"); uint64_t size = uint64_arg(info[1], "size"); auto state = state_; return async_void(info.Env(), "rados_trunc", oid, [state, oid, size] { return state->use([&](rados_ioctx_t h) { return rados_trunc(h, oid.c_str(), size); }); });
}
Napi::Value IoContextWrap::Stat(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"); auto state = state_; return async_value<ObjectStat>(info.Env(), "rados_stat", oid, [state, oid](ObjectStat& out) { return state->use([&](rados_ioctx_t h) { return rados_stat(h, oid.c_str(), &out.size, &out.modified); }); }, [](Napi::Env env, ObjectStat& value) { auto out = Napi::Object::New(env); out.Set("size", Napi::BigInt::New(env, value.size)); out.Set("modifiedAt", Napi::Date::New(env, static_cast<double>(value.modified) * 1000)); return out; });
}

Napi::Value IoContextWrap::SetXattr(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"), name = string_arg(info, 1, "name"); auto data = buffer_arg(info, 2, "value"); auto state = state_; return async_void(info.Env(), "rados_setxattr", oid + "/" + name, [state, oid, name, data] { return state->use([&](rados_ioctx_t h) { return rados_setxattr(h, oid.c_str(), name.c_str(), reinterpret_cast<const char*>(data.data()), data.size()); }); });
}
Napi::Value IoContextWrap::GetXattr(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"), name = string_arg(info, 1, "name"); auto state = state_;
  return async_value<Bytes>(info.Env(), "rados_getxattr", oid + "/" + name, [state, oid, name](Bytes& out) { size_t size = 4096; int rc; do { out.resize(size); rc = state->use([&](rados_ioctx_t h) { return rados_getxattr(h, oid.c_str(), name.c_str(), reinterpret_cast<char*>(out.data()), out.size()); }); size *= 2; } while (rc == -ERANGE && size <= 64 * 1024 * 1024); if (rc >= 0) out.resize(static_cast<size_t>(rc)); return rc < 0 ? rc : 0; }, [](Napi::Env env, Bytes& out) { return Napi::Buffer<uint8_t>::Copy(env, out.data(), out.size()); });
}
Napi::Value IoContextWrap::RemoveXattr(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"), name = string_arg(info, 1, "name"); auto state = state_; return async_void(info.Env(), "rados_rmxattr", oid + "/" + name, [state, oid, name] { return state->use([&](rados_ioctx_t h) { return rados_rmxattr(h, oid.c_str(), name.c_str()); }); });
}
Napi::Value IoContextWrap::GetXattrs(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"); auto state = state_;
  return async_value<ByteMap>(info.Env(), "rados_getxattrs", oid, [state, oid](ByteMap& out) { return state->use([&](rados_ioctx_t h) { rados_xattrs_iter_t iter = nullptr; int rc = rados_getxattrs(h, oid.c_str(), &iter); if (rc < 0) return rc; const char* name = nullptr; const char* value = nullptr; size_t len = 0; while ((rc = rados_getxattrs_next(iter, &name, &value, &len)) == 0 && name) out.emplace_back(name, Bytes(value, value + len)); rados_getxattrs_end(iter); return rc; }); }, [](Napi::Env env, ByteMap& out) { return byte_map_to_js(env, out); });
}

Napi::Value IoContextWrap::GetOmap(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"); auto start = string_arg(info, 1, "startAfter"); auto prefix = string_arg(info, 2, "prefix"); uint64_t max = uint64_arg(info[3], "maxReturn"); auto state = state_;
  return async_value<OmapResult>(info.Env(), "rados_read_op_omap_get_vals2", oid, [state, oid, start, prefix, max](OmapResult& out) { return state->use([&](rados_ioctx_t h) { rados_read_op_t op = rados_create_read_op(); rados_omap_iter_t iter = nullptr; unsigned char more = 0; int op_rc = 0; rados_read_op_omap_get_vals2(op, start.c_str(), prefix.c_str(), max, &iter, &more, &op_rc); int rc = rados_read_op_operate(op, h, oid.c_str(), 0); if (rc >= 0) rc = op_rc; if (rc >= 0) { char* key = nullptr; char* value = nullptr; size_t key_len = 0, value_len = 0; while ((rc = rados_omap_get_next2(iter, &key, &value, &key_len, &value_len)) == 0 && key) out.entries.emplace_back(std::string(key, key_len), Bytes(value, value + value_len)); out.more = more != 0; } if (iter) rados_omap_get_end(iter); rados_release_read_op(op); return rc; }); }, [](Napi::Env env, OmapResult& value) { auto out = Napi::Object::New(env); out.Set("entries", byte_map_to_js(env, value.entries)); out.Set("more", value.more); return out; });
}

Napi::Value IoContextWrap::SetOmap(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"); if (!info[1].IsObject()) throw Napi::TypeError::New(info.Env(), "values must be an object"); auto object = info[1].As<Napi::Object>(); auto names = object.GetPropertyNames(); std::vector<std::string> keys; std::vector<Bytes> values;
  for (uint32_t i = 0; i < names.Length(); ++i) { auto key = names.Get(i).As<Napi::String>().Utf8Value(); auto value = object.Get(key); if (!value.IsBuffer()) throw Napi::TypeError::New(info.Env(), "omap values must be Buffers"); auto b = value.As<Napi::Buffer<uint8_t>>(); keys.push_back(key); values.emplace_back(b.Data(), b.Data() + b.Length()); }
  auto state = state_; return async_void(info.Env(), "rados_write_op_omap_set2", oid, [state, oid, keys, values] { return state->use([&](rados_ioctx_t h) { std::vector<const char*> kp, vp; std::vector<size_t> kl, vl; for (size_t i = 0; i < keys.size(); ++i) { kp.push_back(keys[i].data()); kl.push_back(keys[i].size()); vp.push_back(reinterpret_cast<const char*>(values[i].data())); vl.push_back(values[i].size()); } auto op = rados_create_write_op(); rados_write_op_omap_set2(op, kp.data(), vp.data(), kl.data(), vl.data(), keys.size()); int rc = rados_write_op_operate(op, h, oid.c_str(), nullptr, 0); rados_release_write_op(op); return rc; }); });
}

Napi::Value IoContextWrap::RemoveOmapKeys(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"); if (!info[1].IsArray()) throw Napi::TypeError::New(info.Env(), "keys must be an array"); auto array = info[1].As<Napi::Array>(); std::vector<std::string> keys; for (uint32_t i = 0; i < array.Length(); ++i) keys.push_back(array.Get(i).As<Napi::String>().Utf8Value()); auto state = state_;
  return async_void(info.Env(), "rados_write_op_omap_rm_keys2", oid, [state, oid, keys] { return state->use([&](rados_ioctx_t h) { std::vector<const char*> kp; std::vector<size_t> kl; for (const auto& key : keys) { kp.push_back(key.data()); kl.push_back(key.size()); } auto op = rados_create_write_op(); rados_write_op_omap_rm_keys2(op, kp.data(), kl.data(), keys.size()); int rc = rados_write_op_operate(op, h, oid.c_str(), nullptr, 0); rados_release_write_op(op); return rc; }); });
}

Napi::Value IoContextWrap::ListObjects(const Napi::CallbackInfo& info) {
  auto state = state_; return async_value<std::vector<ObjectEntry>>(info.Env(), "rados_nobjects_list", pool_, [state](std::vector<ObjectEntry>& out) { return state->use([&](rados_ioctx_t h) { rados_list_ctx_t iter = nullptr; int rc = rados_nobjects_list_open(h, &iter); if (rc < 0) return rc; const char *entry = nullptr, *key = nullptr, *space = nullptr; size_t entry_len = 0, key_len = 0, space_len = 0; while ((rc = rados_nobjects_list_next2(iter, &entry, &key, &space, &entry_len, &key_len, &space_len)) == 0) out.push_back({std::string(entry, entry_len), key ? std::string(key, key_len) : "", space ? std::string(space, space_len) : ""}); rados_nobjects_list_close(iter); return rc == -ENOENT ? 0 : rc; }); }, [](Napi::Env env, std::vector<ObjectEntry>& values) { auto out = Napi::Array::New(env, values.size()); for (size_t i = 0; i < values.size(); ++i) { auto item = Napi::Object::New(env); item.Set("name", values[i].name); item.Set("locator", values[i].locator); item.Set("namespace", values[i].nspace); out.Set(i, item); } return out; });
}

Napi::Value IoContextWrap::Notify(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"); auto payload = buffer_arg(info, 1, "payload"); uint64_t timeout = uint64_arg(info[2], "timeoutMs"); auto state = state_;
  return async_value<Bytes>(info.Env(), "rados_notify2", oid, [state, oid, payload, timeout](Bytes& out) { char* reply = nullptr; size_t len = 0; int rc = state->use([&](rados_ioctx_t h) { return rados_notify2(h, oid.c_str(), reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()), timeout, &reply, &len); }); if (reply) { out.assign(reply, reply + len); rados_buffer_free(reply); } return rc; }, [](Napi::Env env, Bytes& out) { return Napi::Buffer<uint8_t>::Copy(env, out.data(), out.size()); });
}
Napi::Value IoContextWrap::Flush(const Napi::CallbackInfo& info) { auto state = state_; return async_void(info.Env(), "rados_aio_flush", pool_, [state] { return state->use([](rados_ioctx_t h) { return rados_aio_flush(h); }); }); }

Napi::Value IoContextWrap::LockExclusive(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"), name = string_arg(info, 1, "name"), cookie = string_arg(info, 2, "cookie"), desc = string_arg(info, 3, "description"); uint64_t duration = uint64_arg(info[4], "durationMs"); uint8_t flags = static_cast<uint8_t>(uint64_arg(info[5], "flags")); timeval tv{static_cast<time_t>(duration / 1000), static_cast<suseconds_t>((duration % 1000) * 1000)}; auto state = state_; return async_void(info.Env(), "rados_lock_exclusive", oid + "/" + name, [state, oid, name, cookie, desc, tv, flags]() mutable { return state->use([&](rados_ioctx_t h) { return rados_lock_exclusive(h, oid.c_str(), name.c_str(), cookie.c_str(), desc.c_str(), &tv, flags); }); });
}
Napi::Value IoContextWrap::LockShared(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid"), name = string_arg(info, 1, "name"), cookie = string_arg(info, 2, "cookie"), tag = string_arg(info, 3, "tag"), desc = string_arg(info, 4, "description"); uint64_t duration = uint64_arg(info[5], "durationMs"); uint8_t flags = static_cast<uint8_t>(uint64_arg(info[6], "flags")); timeval tv{static_cast<time_t>(duration / 1000), static_cast<suseconds_t>((duration % 1000) * 1000)}; auto state = state_; return async_void(info.Env(), "rados_lock_shared", oid + "/" + name, [state, oid, name, cookie, tag, desc, tv, flags]() mutable { return state->use([&](rados_ioctx_t h) { return rados_lock_shared(h, oid.c_str(), name.c_str(), cookie.c_str(), tag.c_str(), desc.c_str(), &tv, flags); }); });
}
Napi::Value IoContextWrap::Unlock(const Napi::CallbackInfo& info) { auto oid = string_arg(info, 0, "oid"), name = string_arg(info, 1, "name"), cookie = string_arg(info, 2, "cookie"); auto state = state_; return async_void(info.Env(), "rados_unlock", oid + "/" + name, [state, oid, name, cookie] { return state->use([&](rados_ioctx_t h) { return rados_unlock(h, oid.c_str(), name.c_str(), cookie.c_str()); }); }); }
Napi::Value IoContextWrap::BreakLock(const Napi::CallbackInfo& info) { auto oid = string_arg(info, 0, "oid"), name = string_arg(info, 1, "name"), client = string_arg(info, 2, "client"), cookie = string_arg(info, 3, "cookie"); auto state = state_; return async_void(info.Env(), "rados_break_lock", oid + "/" + name, [state, oid, name, client, cookie] { return state->use([&](rados_ioctx_t h) { return rados_break_lock(h, oid.c_str(), name.c_str(), client.c_str(), cookie.c_str()); }); }); }
Napi::Value IoContextWrap::Rbd(const Napi::CallbackInfo& info) { return RbdPoolWrap::NewInstance(info.Env(), state_, cluster_, pool_); }
Napi::Value IoContextWrap::Watch(const Napi::CallbackInfo& info) {
  auto oid = string_arg(info, 0, "oid");
  uint32_t timeout = static_cast<uint32_t>(uint64_arg(info[1], "timeoutSeconds"));
  uint32_t queue = static_cast<uint32_t>(uint64_arg(info[2], "queueSize"));
  if (info.Length() < 4 || !info[3].IsFunction()) throw Napi::TypeError::New(info.Env(), "callback must be a function");
  return ObjectWatchWrap::Create(info.Env(), state_, cluster_, oid, timeout, queue, info[3].As<Napi::Function>());
}

}  // namespace nl
