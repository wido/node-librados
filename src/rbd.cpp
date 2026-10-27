#include "rbd.h"
#include "events.h"

#include <cstring>
#include <map>

namespace nl {

Napi::FunctionReference RbdPoolWrap::constructor;
Napi::FunctionReference RbdImageWrap::constructor;

namespace {
using Bytes = std::vector<uint8_t>;
struct ImageSpec { std::string id; std::string name; };
struct OpenImage { std::shared_ptr<ImageState> state; std::string name; };
struct ImageStat { uint64_t size{}; uint64_t object_size{}; uint64_t objects{}; int order{}; };
struct Snap { uint64_t id{}; uint64_t size{}; std::string name; };
using StringMap = std::vector<std::pair<std::string, std::string>>;

Bytes data_arg(const Napi::CallbackInfo& info, size_t index, const char* name) {
  if (info.Length() <= index || !info[index].IsBuffer()) throw Napi::TypeError::New(info.Env(), std::string(name) + " must be a Buffer");
  auto b = info[index].As<Napi::Buffer<uint8_t>>(); return {b.Data(), b.Data() + b.Length()};
}

std::vector<std::string> nul_strings(const char* data, size_t length) {
  std::vector<std::string> out;
  for (const char* p = data; p < data + length && *p; p += std::strlen(p) + 1) out.emplace_back(p);
  return out;
}

int ignore_progress(uint64_t, uint64_t, void*) { return 0; }
}

void RbdPoolWrap::Init(Napi::Env env, Napi::Object exports) {
  auto function = DefineClass(env, "NativeRbdPool", {
      InstanceMethod("list", &RbdPoolWrap::List),
      InstanceMethod("create", &RbdPoolWrap::Create),
      InstanceMethod("remove", &RbdPoolWrap::Remove),
      InstanceMethod("rename", &RbdPoolWrap::Rename),
      InstanceMethod("open", &RbdPoolWrap::Open),
      InstanceMethod("clone", &RbdPoolWrap::Clone),
      InstanceMethod("trashMove", &RbdPoolWrap::TrashMove),
      InstanceMethod("trashRestore", &RbdPoolWrap::TrashRestore),
      InstanceMethod("trashRemove", &RbdPoolWrap::TrashRemove),
      InstanceMethod("namespaceList", &RbdPoolWrap::NamespaceList),
      InstanceMethod("namespaceCreate", &RbdPoolWrap::NamespaceCreate),
      InstanceMethod("namespaceRemove", &RbdPoolWrap::NamespaceRemove),
      InstanceMethod("stats", &RbdPoolWrap::PoolStats),
      InstanceMethod("advanced", &RbdPoolWrap::Advanced),
  });
  constructor = Napi::Persistent(function); constructor.SuppressDestruct(); exports.Set("NativeRbdPool", function);
}

Napi::Object RbdPoolWrap::NewInstance(Napi::Env env, std::shared_ptr<IoState> io,
                                      std::shared_ptr<ClusterState> cluster,
                                      std::string pool) {
  using Init = std::tuple<std::shared_ptr<IoState>, std::shared_ptr<ClusterState>, std::string>;
  auto* init = new Init(std::move(io), std::move(cluster), std::move(pool)); return constructor.New({Napi::External<Init>::New(env, init)});
}
RbdPoolWrap::RbdPoolWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<RbdPoolWrap>(info) {
  using Init = std::tuple<std::shared_ptr<IoState>, std::shared_ptr<ClusterState>, std::string>;
  if (!info.Length() || !info[0].IsExternal())
    throw Napi::TypeError::New(info.Env(), "RbdPool cannot be constructed directly");
  auto* init = info[0].As<Napi::External<Init>>().Data();
  io_ = std::move(std::get<0>(*init));
  cluster_ = std::move(std::get<1>(*init));
  pool_ = std::move(std::get<2>(*init));
  delete init;
}

Napi::Value RbdPoolWrap::List(const Napi::CallbackInfo& info) {
  auto io = io_; auto busy = in_flight(io_); return async_value<std::vector<ImageSpec>>(info.Env(), "rbd_list2", pool_, [busy, io](std::vector<ImageSpec>& out) { return io->use([&](rados_ioctx_t h) { size_t count = 0; int rc = rbd_list2(h, nullptr, &count); if (rc < 0 && rc != -ERANGE) return rc; if (!count) return 0; std::vector<rbd_image_spec_t> images(count); rc = rbd_list2(h, images.data(), &count); if (rc >= 0) { for (size_t i = 0; i < count; ++i) out.push_back({images[i].id ? images[i].id : "", images[i].name ? images[i].name : ""}); rbd_image_spec_list_cleanup(images.data(), count); } return rc; }); }, [](Napi::Env env, std::vector<ImageSpec>& values) { auto out = Napi::Array::New(env, values.size()); for (size_t i = 0; i < values.size(); ++i) { auto item = Napi::Object::New(env); item.Set("id", values[i].id); item.Set("name", values[i].name); out.Set(i, item); } return out; });
}
Napi::Value RbdPoolWrap::Create(const Napi::CallbackInfo& info) {
  auto name = string_arg(info, 0, "name"); uint64_t size = uint64_arg(info[1], "size"); uint64_t features = uint64_arg(info[2], "features"); int order = static_cast<int>(uint64_arg(info[3], "order")); auto io = io_; auto busy = in_flight(io_); return async_value<int>(info.Env(), "rbd_create2", name, [busy, io, name, size, features, order](int& actual) mutable { actual = order; return io->use([&](rados_ioctx_t h) { return rbd_create2(h, name.c_str(), size, features, &actual); }); }, [](Napi::Env env, int& value) { return Napi::Number::New(env, value); });
}
Napi::Value RbdPoolWrap::Remove(const Napi::CallbackInfo& info) { auto name = string_arg(info, 0, "name"); auto io = io_; auto busy = in_flight(io_); return async_void(info.Env(), "rbd_remove", name, [busy, io, name] { return io->use([&](rados_ioctx_t h) { return rbd_remove(h, name.c_str()); }); }); }
Napi::Value RbdPoolWrap::Rename(const Napi::CallbackInfo& info) { auto from = string_arg(info, 0, "from"), to = string_arg(info, 1, "to"); auto io = io_; auto busy = in_flight(io_); return async_void(info.Env(), "rbd_rename", from, [busy, io, from, to] { return io->use([&](rados_ioctx_t h) { return rbd_rename(h, from.c_str(), to.c_str()); }); }); }
Napi::Value RbdPoolWrap::Open(const Napi::CallbackInfo& info) {
  auto name = string_arg(info, 0, "name"); std::optional<std::string> snap; if (info.Length() > 1 && !info[1].IsNull() && !info[1].IsUndefined()) snap = string_arg(info, 1, "snapshot"); bool read_only = info.Length() > 2 && info[2].ToBoolean(); auto io = io_; auto busy = in_flight(io_);
  return async_value<OpenImage>(info.Env(), "rbd_open", name, [busy, io, name, snap, read_only](OpenImage& out) { rbd_image_t handle = nullptr; int rc = io->use([&](rados_ioctx_t h) { return read_only ? rbd_open_read_only(h, name.c_str(), &handle, snap ? snap->c_str() : nullptr) : rbd_open(h, name.c_str(), &handle, snap ? snap->c_str() : nullptr); }); if (rc >= 0) { io->add_child(); out.state = std::make_shared<ImageState>(handle, [io](rbd_image_t image) { int result = rbd_close(image); io->remove_child(); return result; }); out.name = name; } return rc; }, [io](Napi::Env env, OpenImage& value) { return RbdImageWrap::NewInstance(env, std::move(value.state), io, value.name); });
}
Napi::Value RbdPoolWrap::Clone(const Napi::CallbackInfo& info) { auto parent = string_arg(info, 0, "parent"), snap = string_arg(info, 1, "snapshot"), child = string_arg(info, 2, "child"); uint64_t features = uint64_arg(info[3], "features"); int order = static_cast<int>(uint64_arg(info[4], "order")); auto io = io_; auto busy = in_flight(io_); return async_value<int>(info.Env(), "rbd_clone", child, [busy, io, parent, snap, child, features, order](int& actual) mutable { actual = order; return io->use([&](rados_ioctx_t h) { return rbd_clone(h, parent.c_str(), snap.c_str(), h, child.c_str(), features, &actual); }); }, [](Napi::Env env, int& actual) { return Napi::Number::New(env, actual); }); }
Napi::Value RbdPoolWrap::TrashMove(const Napi::CallbackInfo& info) { auto name = string_arg(info, 0, "name"); uint64_t delay = uint64_arg(info[1], "delaySeconds"); auto io = io_; auto busy = in_flight(io_); return async_void(info.Env(), "rbd_trash_move", name, [busy, io, name, delay] { return io->use([&](rados_ioctx_t h) { return rbd_trash_move(h, name.c_str(), delay); }); }); }
Napi::Value RbdPoolWrap::TrashRestore(const Napi::CallbackInfo& info) { auto id = string_arg(info, 0, "id"), name = string_arg(info, 1, "name"); auto io = io_; auto busy = in_flight(io_); return async_void(info.Env(), "rbd_trash_restore", id, [busy, io, id, name] { return io->use([&](rados_ioctx_t h) { return rbd_trash_restore(h, id.c_str(), name.c_str()); }); }); }
Napi::Value RbdPoolWrap::TrashRemove(const Napi::CallbackInfo& info) { auto id = string_arg(info, 0, "id"); bool force = info.Length() > 1 && info[1].ToBoolean(); auto io = io_; auto busy = in_flight(io_); return async_void(info.Env(), "rbd_trash_remove", id, [busy, io, id, force] { return io->use([&](rados_ioctx_t h) { return rbd_trash_remove(h, id.c_str(), force); }); }); }

Napi::Value RbdPoolWrap::NamespaceList(const Napi::CallbackInfo& info) {
  auto io = io_; auto busy = in_flight(io_); return async_value<std::vector<std::string>>(info.Env(), "rbd_namespace_list", pool_, [busy, io](std::vector<std::string>& out) { return io->use([&](rados_ioctx_t h) { size_t size = 0; int rc = rbd_namespace_list(h, nullptr, &size); if (rc < 0 && rc != -ERANGE) return rc; std::vector<char> data(size); rc = rbd_namespace_list(h, data.data(), &size); if (rc >= 0) out = nul_strings(data.data(), size); return rc; }); }, [](Napi::Env env, std::vector<std::string>& values) { auto out = Napi::Array::New(env, values.size()); for (size_t i = 0; i < values.size(); ++i) out.Set(i, values[i]); return out; });
}
Napi::Value RbdPoolWrap::NamespaceCreate(const Napi::CallbackInfo& info) { auto name = string_arg(info, 0, "name"); auto io = io_; auto busy = in_flight(io_); return async_void(info.Env(), "rbd_namespace_create", name, [busy, io, name] { return io->use([&](rados_ioctx_t h) { return rbd_namespace_create(h, name.c_str()); }); }); }
Napi::Value RbdPoolWrap::NamespaceRemove(const Napi::CallbackInfo& info) { auto name = string_arg(info, 0, "name"); auto io = io_; auto busy = in_flight(io_); return async_void(info.Env(), "rbd_namespace_remove", name, [busy, io, name] { return io->use([&](rados_ioctx_t h) { return rbd_namespace_remove(h, name.c_str()); }); }); }
Napi::Value RbdPoolWrap::PoolStats(const Napi::CallbackInfo& info) {
  using Stats = std::array<uint64_t, 8>; auto io = io_; auto busy = in_flight(io_); return async_value<Stats>(info.Env(), "rbd_pool_stats_get", pool_, [busy, io](Stats& out) { return io->use([&](rados_ioctx_t h) { rbd_pool_stats_t stats = nullptr; rbd_pool_stats_create(&stats); for (int i = 0; i < 8; ++i) rbd_pool_stats_option_add_uint64(stats, RBD_POOL_STAT_OPTION_IMAGES + i, &out[i]); int rc = rbd_pool_stats_get(h, stats); rbd_pool_stats_destroy(stats); return rc; }); }, [](Napi::Env env, Stats& s) { const char* names[] = {"images", "imageProvisionedBytes", "imageMaxProvisionedBytes", "imageSnapshots", "trashImages", "trashProvisionedBytes", "trashMaxProvisionedBytes", "trashSnapshots"}; auto out = Napi::Object::New(env); for (size_t i = 0; i < 8; ++i) out.Set(names[i], Napi::BigInt::New(env, s[i])); return out; });
}

Napi::Value RbdPoolWrap::Advanced(const Napi::CallbackInfo& info) {
  auto operation = string_arg(info, 0, "operation");
  if (!info[1].IsArray() || !info[2].IsArray()) throw Napi::TypeError::New(info.Env(), "strings and values must be arrays");
  auto sa = info[1].As<Napi::Array>(), va = info[2].As<Napi::Array>();
  std::vector<std::string> strings; std::vector<uint64_t> values;
  for (uint32_t i = 0; i < sa.Length(); ++i) strings.push_back(sa.Get(i).As<Napi::String>().Utf8Value());
  for (uint32_t i = 0; i < va.Length(); ++i) values.push_back(uint64_arg(va.Get(i), "value"));
  auto io = io_; auto busy = in_flight(io_);
  return async_value<int64_t>(info.Env(), "rbd_" + operation, pool_, [busy, io, operation, strings, values](int64_t& out) {
    return io->use([&](rados_ioctx_t h) {
      int rc = -EINVAL;
      if (operation == "mirrorModeGet") { rbd_mirror_mode_t mode{}; rc = rbd_mirror_mode_get(h, &mode); out = mode; }
      else if (operation == "mirrorModeSet" && !values.empty()) rc = rbd_mirror_mode_set(h, static_cast<rbd_mirror_mode_t>(values[0]));
      else if (operation == "migrationPrepare" && strings.size() >= 2) { rbd_image_options_t opts{}; rbd_image_options_create(&opts); rc = rbd_migration_prepare(h, strings[0].c_str(), h, strings[1].c_str(), opts); rbd_image_options_destroy(opts); }
      else if (operation == "migrationExecute" && !strings.empty()) rc = rbd_migration_execute(h, strings[0].c_str());
      else if (operation == "migrationCommit" && !strings.empty()) rc = rbd_migration_commit(h, strings[0].c_str());
      else if (operation == "migrationAbort" && !strings.empty()) rc = rbd_migration_abort(h, strings[0].c_str());
      else if (operation == "groupCreate" && !strings.empty()) rc = rbd_group_create(h, strings[0].c_str());
      else if (operation == "groupRemove" && !strings.empty()) rc = rbd_group_remove(h, strings[0].c_str());
      else if (operation == "groupRename" && strings.size() >= 2) rc = rbd_group_rename(h, strings[0].c_str(), strings[1].c_str());
      else if (operation == "groupImageAdd" && strings.size() >= 2) rc = rbd_group_image_add(h, strings[0].c_str(), h, strings[1].c_str());
      else if (operation == "groupImageRemove" && strings.size() >= 2) rc = rbd_group_image_remove(h, strings[0].c_str(), h, strings[1].c_str());
      else if (operation == "groupSnapshotCreate" && strings.size() >= 2) rc = rbd_group_snap_create(h, strings[0].c_str(), strings[1].c_str());
      else if (operation == "groupSnapshotRemove" && strings.size() >= 2) rc = rbd_group_snap_remove(h, strings[0].c_str(), strings[1].c_str());
      else if (operation == "groupSnapshotRollback" && strings.size() >= 2) rc = rbd_group_snap_rollback(h, strings[0].c_str(), strings[1].c_str());
      return rc;
    });
  }, [](Napi::Env env, int64_t& value) { return Napi::BigInt::New(env, value); });
}

void RbdImageWrap::Init(Napi::Env env, Napi::Object exports) {
  auto function = DefineClass(env, "NativeRbdImage", {
      InstanceMethod("close", &RbdImageWrap::Close), InstanceAccessor("closed", &RbdImageWrap::IsClosed, nullptr),
      InstanceMethod("stat", &RbdImageWrap::Stat), InstanceMethod("size", &RbdImageWrap::Size), InstanceMethod("resize", &RbdImageWrap::Resize), InstanceMethod("features", &RbdImageWrap::Features), InstanceMethod("flags", &RbdImageWrap::Flags),
      InstanceMethod("read", &RbdImageWrap::Read), InstanceMethod("write", &RbdImageWrap::Write), InstanceMethod("discard", &RbdImageWrap::Discard), InstanceMethod("flush", &RbdImageWrap::Flush), InstanceMethod("invalidateCache", &RbdImageWrap::InvalidateCache),
      InstanceMethod("snapCreate", &RbdImageWrap::SnapCreate), InstanceMethod("snapRemove", &RbdImageWrap::SnapRemove), InstanceMethod("snapList", &RbdImageWrap::SnapList), InstanceMethod("snapSet", &RbdImageWrap::SnapSet), InstanceMethod("snapProtect", &RbdImageWrap::SnapProtect), InstanceMethod("snapUnprotect", &RbdImageWrap::SnapUnprotect), InstanceMethod("snapRollback", &RbdImageWrap::SnapRollback), InstanceMethod("flatten", &RbdImageWrap::Flatten),
      InstanceMethod("metadataGet", &RbdImageWrap::MetadataGet), InstanceMethod("metadataSet", &RbdImageWrap::MetadataSet), InstanceMethod("metadataRemove", &RbdImageWrap::MetadataRemove), InstanceMethod("metadataList", &RbdImageWrap::MetadataList),
      InstanceMethod("lockAcquire", &RbdImageWrap::LockAcquire), InstanceMethod("lockRelease", &RbdImageWrap::LockRelease), InstanceMethod("lockBreak", &RbdImageWrap::LockBreak), InstanceMethod("lockOwner", &RbdImageWrap::LockOwner),
      InstanceMethod("watchUpdates", &RbdImageWrap::WatchUpdates),
      InstanceMethod("advanced", &RbdImageWrap::Advanced),
  }); constructor = Napi::Persistent(function); constructor.SuppressDestruct(); exports.Set("NativeRbdImage", function);
}
Napi::Object RbdImageWrap::NewInstance(Napi::Env env, std::shared_ptr<ImageState> image, std::shared_ptr<IoState> io, std::string name) { using Init = std::tuple<std::shared_ptr<ImageState>, std::shared_ptr<IoState>, std::string>; auto* init = new Init(std::move(image), std::move(io), std::move(name)); return constructor.New({Napi::External<Init>::New(env, init)}); }
RbdImageWrap::RbdImageWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<RbdImageWrap>(info) { using Init = std::tuple<std::shared_ptr<ImageState>, std::shared_ptr<IoState>, std::string>; if (!info.Length() || !info[0].IsExternal()) throw Napi::TypeError::New(info.Env(), "RbdImage cannot be constructed directly"); auto* init = info[0].As<Napi::External<Init>>().Data(); image_ = std::move(std::get<0>(*init)); io_ = std::move(std::get<1>(*init)); name_ = std::move(std::get<2>(*init)); delete init; }

Napi::Value RbdImageWrap::Close(const Napi::CallbackInfo& info) { auto image = image_; return async_void(info.Env(), "rbd_close", name_, [image] { return image->close_checked(); }); }
Napi::Value RbdImageWrap::IsClosed(const Napi::CallbackInfo& info) { return Napi::Boolean::New(info.Env(), image_->closed()); }
Napi::Value RbdImageWrap::Stat(const Napi::CallbackInfo& info) { auto image = image_; return async_value<ImageStat>(info.Env(), "rbd_stat", name_, [image](ImageStat& out) { rbd_image_info_t value{}; int rc = image->use([&](rbd_image_t h) { return rbd_stat(h, &value, sizeof(value)); }); if (rc >= 0) out = {value.size, value.obj_size, value.num_objs, value.order}; return rc; }, [](Napi::Env env, ImageStat& value) { auto out = Napi::Object::New(env); out.Set("size", Napi::BigInt::New(env, value.size)); out.Set("objectSize", Napi::BigInt::New(env, value.object_size)); out.Set("objects", Napi::BigInt::New(env, value.objects)); out.Set("order", value.order); return out; }); }
Napi::Value RbdImageWrap::Size(const Napi::CallbackInfo& info) { auto image = image_; return async_value<uint64_t>(info.Env(), "rbd_get_size", name_, [image](uint64_t& out) { return image->use([&](rbd_image_t h) { return rbd_get_size(h, &out); }); }, [](Napi::Env env, uint64_t& out) { return Napi::BigInt::New(env, out); }); }
Napi::Value RbdImageWrap::Resize(const Napi::CallbackInfo& info) { uint64_t size = uint64_arg(info[0], "size"); bool shrink = info.Length() > 1 && info[1].ToBoolean(); auto image = image_; return async_void(info.Env(), "rbd_resize2", name_, [image, size, shrink] { return image->use([&](rbd_image_t h) { return rbd_resize2(h, size, shrink, ignore_progress, nullptr); }); }); }
Napi::Value RbdImageWrap::Features(const Napi::CallbackInfo& info) { auto image = image_; return async_value<uint64_t>(info.Env(), "rbd_get_features", name_, [image](uint64_t& out) { return image->use([&](rbd_image_t h) { return rbd_get_features(h, &out); }); }, [](Napi::Env env, uint64_t& out) { return Napi::BigInt::New(env, out); }); }
Napi::Value RbdImageWrap::Flags(const Napi::CallbackInfo& info) { auto image = image_; return async_value<uint64_t>(info.Env(), "rbd_get_flags", name_, [image](uint64_t& out) { return image->use([&](rbd_image_t h) { return rbd_get_flags(h, &out); }); }, [](Napi::Env env, uint64_t& out) { return Napi::BigInt::New(env, out); }); }
Napi::Value RbdImageWrap::Read(const Napi::CallbackInfo& info) { uint64_t offset = uint64_arg(info[0], "offset"); size_t length = static_cast<size_t>(uint64_arg(info[1], "length")); int flags = info.Length() > 2 ? static_cast<int>(int64_arg(info[2], "flags")) : 0; auto image = image_; return async_value<Bytes>(info.Env(), "rbd_read2", name_, [image, offset, length, flags](Bytes& out) { out.resize(length); ssize_t rc = image->use([&](rbd_image_t h) { return rbd_read2(h, offset, length, reinterpret_cast<char*>(out.data()), flags); }); if (rc >= 0) out.resize(static_cast<size_t>(rc)); return rc < 0 ? static_cast<int>(rc) : 0; }, [](Napi::Env env, Bytes& out) { return Napi::Buffer<uint8_t>::Copy(env, out.data(), out.size()); }); }
Napi::Value RbdImageWrap::Write(const Napi::CallbackInfo& info) { uint64_t offset = uint64_arg(info[0], "offset"); auto data = data_arg(info, 1, "data"); int flags = info.Length() > 2 ? static_cast<int>(int64_arg(info[2], "flags")) : 0; auto image = image_; return async_void(info.Env(), "rbd_write2", name_, [image, offset, data, flags] { ssize_t rc = image->use([&](rbd_image_t h) { return rbd_write2(h, offset, data.size(), reinterpret_cast<const char*>(data.data()), flags); }); return rc < 0 ? static_cast<int>(rc) : 0; }); }
Napi::Value RbdImageWrap::Discard(const Napi::CallbackInfo& info) { uint64_t offset = uint64_arg(info[0], "offset"), length = uint64_arg(info[1], "length"); auto image = image_; return async_void(info.Env(), "rbd_discard", name_, [image, offset, length] { return image->use([&](rbd_image_t h) { return rbd_discard(h, offset, length); }); }); }
Napi::Value RbdImageWrap::Flush(const Napi::CallbackInfo& info) { auto image = image_; return async_void(info.Env(), "rbd_flush", name_, [image] { return image->use([](rbd_image_t h) { return rbd_flush(h); }); }); }
Napi::Value RbdImageWrap::InvalidateCache(const Napi::CallbackInfo& info) { auto image = image_; return async_void(info.Env(), "rbd_invalidate_cache", name_, [image] { return image->use([](rbd_image_t h) { return rbd_invalidate_cache(h); }); }); }

Napi::Value RbdImageWrap::SnapCreate(const Napi::CallbackInfo& info) { auto snap = string_arg(info, 0, "snapshot"); auto image = image_; return async_void(info.Env(), "rbd_snap_create", name_ + "@" + snap, [image, snap] { return image->use([&](rbd_image_t h) { return rbd_snap_create(h, snap.c_str()); }); }); }
Napi::Value RbdImageWrap::SnapRemove(const Napi::CallbackInfo& info) { auto snap = string_arg(info, 0, "snapshot"); auto image = image_; return async_void(info.Env(), "rbd_snap_remove", name_ + "@" + snap, [image, snap] { return image->use([&](rbd_image_t h) { return rbd_snap_remove(h, snap.c_str()); }); }); }
Napi::Value RbdImageWrap::SnapList(const Napi::CallbackInfo& info) { auto image = image_; return async_value<std::vector<Snap>>(info.Env(), "rbd_snap_list", name_, [image](std::vector<Snap>& out) { return image->use([&](rbd_image_t h) { int count = 0; int rc = rbd_snap_list(h, nullptr, &count); if (rc < 0 && rc != -ERANGE) return rc; if (!count) return 0; std::vector<rbd_snap_info_t> snaps(static_cast<size_t>(count)); rc = rbd_snap_list(h, snaps.data(), &count); if (rc >= 0) { for (int i = 0; i < count; ++i) out.push_back({snaps[i].id, snaps[i].size, snaps[i].name ? snaps[i].name : ""}); rbd_snap_list_end(snaps.data()); } return rc; }); }, [](Napi::Env env, std::vector<Snap>& snaps) { auto out = Napi::Array::New(env, snaps.size()); for (size_t i = 0; i < snaps.size(); ++i) { auto item = Napi::Object::New(env); item.Set("id", Napi::BigInt::New(env, snaps[i].id)); item.Set("size", Napi::BigInt::New(env, snaps[i].size)); item.Set("name", snaps[i].name); out.Set(i, item); } return out; }); }
Napi::Value RbdImageWrap::SnapSet(const Napi::CallbackInfo& info) { std::optional<std::string> snap; if (info.Length() && !info[0].IsNull()) snap = string_arg(info, 0, "snapshot"); auto image = image_; return async_void(info.Env(), "rbd_snap_set", name_, [image, snap] { return image->use([&](rbd_image_t h) { return rbd_snap_set(h, snap ? snap->c_str() : nullptr); }); }); }
Napi::Value RbdImageWrap::SnapProtect(const Napi::CallbackInfo& info) { auto snap = string_arg(info, 0, "snapshot"); auto image = image_; return async_void(info.Env(), "rbd_snap_protect", name_ + "@" + snap, [image, snap] { return image->use([&](rbd_image_t h) { return rbd_snap_protect(h, snap.c_str()); }); }); }
Napi::Value RbdImageWrap::SnapUnprotect(const Napi::CallbackInfo& info) { auto snap = string_arg(info, 0, "snapshot"); auto image = image_; return async_void(info.Env(), "rbd_snap_unprotect", name_ + "@" + snap, [image, snap] { return image->use([&](rbd_image_t h) { return rbd_snap_unprotect(h, snap.c_str()); }); }); }
Napi::Value RbdImageWrap::SnapRollback(const Napi::CallbackInfo& info) { auto snap = string_arg(info, 0, "snapshot"); auto image = image_; return async_void(info.Env(), "rbd_snap_rollback", name_ + "@" + snap, [image, snap] { return image->use([&](rbd_image_t h) { return rbd_snap_rollback(h, snap.c_str()); }); }); }
Napi::Value RbdImageWrap::Flatten(const Napi::CallbackInfo& info) { auto image = image_; return async_void(info.Env(), "rbd_flatten", name_, [image] { return image->use([](rbd_image_t h) { return rbd_flatten(h); }); }); }

Napi::Value RbdImageWrap::MetadataGet(const Napi::CallbackInfo& info) { auto key = string_arg(info, 0, "key"); auto image = image_; return async_value<std::string>(info.Env(), "rbd_metadata_get", name_ + "/" + key, [image, key](std::string& out) { size_t size = 256; int rc; do { std::vector<char> data(size); size_t actual = size; rc = image->use([&](rbd_image_t h) { return rbd_metadata_get(h, key.c_str(), data.data(), &actual); }); if (rc >= 0) { out.assign(data.data(), strnlen(data.data(), actual)); return 0; } size = actual > size ? actual : size * 2; } while (rc == -ERANGE); return rc; }, [](Napi::Env env, std::string& out) { return Napi::String::New(env, out); }); }
Napi::Value RbdImageWrap::MetadataSet(const Napi::CallbackInfo& info) { auto key = string_arg(info, 0, "key"), value = string_arg(info, 1, "value"); auto image = image_; return async_void(info.Env(), "rbd_metadata_set", name_ + "/" + key, [image, key, value] { return image->use([&](rbd_image_t h) { return rbd_metadata_set(h, key.c_str(), value.c_str()); }); }); }
Napi::Value RbdImageWrap::MetadataRemove(const Napi::CallbackInfo& info) { auto key = string_arg(info, 0, "key"); auto image = image_; return async_void(info.Env(), "rbd_metadata_remove", name_ + "/" + key, [image, key] { return image->use([&](rbd_image_t h) { return rbd_metadata_remove(h, key.c_str()); }); }); }
Napi::Value RbdImageWrap::MetadataList(const Napi::CallbackInfo& info) { auto start = string_arg(info, 0, "startAfter"); uint64_t max = uint64_arg(info[1], "maxReturn"); auto image = image_; return async_value<StringMap>(info.Env(), "rbd_metadata_list", name_, [image, start, max](StringMap& out) { return image->use([&](rbd_image_t h) { size_t kl = 0, vl = 0; int rc = rbd_metadata_list(h, start.c_str(), max, nullptr, &kl, nullptr, &vl); if (rc < 0 && rc != -ERANGE) return rc; std::vector<char> keys(kl), values(vl); rc = rbd_metadata_list(h, start.c_str(), max, keys.data(), &kl, values.data(), &vl); if (rc >= 0) { auto ks = nul_strings(keys.data(), kl), vs = nul_strings(values.data(), vl); for (size_t i = 0; i < ks.size() && i < vs.size(); ++i) out.emplace_back(ks[i], vs[i]); } return rc; }); }, [](Napi::Env env, StringMap& values) { auto out = Napi::Object::New(env); for (const auto& [k, v] : values) out.Set(k, v); return out; }); }

Napi::Value RbdImageWrap::LockAcquire(const Napi::CallbackInfo& info) { auto mode = static_cast<rbd_lock_mode_t>(int64_arg(info[0], "mode")); auto image = image_; return async_void(info.Env(), "rbd_lock_acquire", name_, [image, mode] { return image->use([&](rbd_image_t h) { return rbd_lock_acquire(h, mode); }); }); }
Napi::Value RbdImageWrap::LockRelease(const Napi::CallbackInfo& info) { auto image = image_; return async_void(info.Env(), "rbd_lock_release", name_, [image] { return image->use([](rbd_image_t h) { return rbd_lock_release(h); }); }); }
Napi::Value RbdImageWrap::LockBreak(const Napi::CallbackInfo& info) { auto mode = static_cast<rbd_lock_mode_t>(int64_arg(info[0], "mode")); auto owner = string_arg(info, 1, "owner"); auto image = image_; return async_void(info.Env(), "rbd_lock_break", name_, [image, mode, owner] { return image->use([&](rbd_image_t h) { return rbd_lock_break(h, mode, owner.c_str()); }); }); }
Napi::Value RbdImageWrap::LockOwner(const Napi::CallbackInfo& info) { auto image = image_; return async_value<bool>(info.Env(), "rbd_is_exclusive_lock_owner", name_, [image](bool& out) { int value = 0; int rc = image->use([&](rbd_image_t h) { return rbd_is_exclusive_lock_owner(h, &value); }); out = value != 0; return rc; }, [](Napi::Env env, bool& out) { return Napi::Boolean::New(env, out); }); }
Napi::Value RbdImageWrap::WatchUpdates(const Napi::CallbackInfo& info) { uint32_t queue = static_cast<uint32_t>(uint64_arg(info[0], "queueSize")); if (info.Length() < 2 || !info[1].IsFunction()) throw Napi::TypeError::New(info.Env(), "callback must be a function"); return ImageUpdateWatchWrap::Create(info.Env(), image_, queue, info[1].As<Napi::Function>()); }
Napi::Value RbdImageWrap::Advanced(const Napi::CallbackInfo& info) {
  auto operation = string_arg(info, 0, "operation");
  if (!info[1].IsArray() || !info[2].IsArray()) throw Napi::TypeError::New(info.Env(), "strings and values must be arrays");
  auto sa = info[1].As<Napi::Array>(), va = info[2].As<Napi::Array>(); std::vector<std::string> strings; std::vector<uint64_t> values;
  for (uint32_t i = 0; i < sa.Length(); ++i) strings.push_back(sa.Get(i).As<Napi::String>().Utf8Value());
  for (uint32_t i = 0; i < va.Length(); ++i) values.push_back(uint64_arg(va.Get(i), "value"));
  auto image = image_;
  return async_value<int64_t>(info.Env(), "rbd_" + operation, name_, [image, operation, strings, values](int64_t& out) {
    return image->use([&](rbd_image_t h) {
      int rc = -EINVAL;
      if (operation == "mirrorEnable" && !values.empty()) rc = rbd_mirror_image_enable2(h, static_cast<rbd_mirror_image_mode_t>(values[0]));
      else if (operation == "mirrorDisable") rc = rbd_mirror_image_disable(h, !values.empty() && values[0]);
      else if (operation == "mirrorPromote") rc = rbd_mirror_image_promote(h, !values.empty() && values[0]);
      else if (operation == "mirrorDemote") rc = rbd_mirror_image_demote(h);
      else if (operation == "mirrorResync") rc = rbd_mirror_image_resync(h);
      else if (operation == "mirrorSnapshot") { uint64_t id = 0; rc = rbd_mirror_image_create_snapshot(h, &id); out = static_cast<int64_t>(id); }
      else if (operation == "sparsify" && !values.empty()) rc = rbd_sparsify(h, values[0]);
      else if (operation == "rebuildObjectMap") rc = rbd_rebuild_object_map(h, ignore_progress, nullptr);
      else if (operation == "updateFeatures" && values.size() >= 2) rc = rbd_update_features(h, values[0], values[1] ? 1 : 0);
      else if ((operation == "encryptionFormat" || operation == "encryptionLoad") && strings.size() >= 1 && values.size() >= 2) {
        rbd_encryption_luks2_format_options_t opts{static_cast<rbd_encryption_algorithm_t>(values[1]), strings[0].data(), strings[0].size()};
        auto format = static_cast<rbd_encryption_format_t>(values[0]);
        rc = operation == "encryptionFormat" ? rbd_encryption_format(h, format, &opts, sizeof(opts)) : rbd_encryption_load(h, format, &opts, sizeof(opts));
      }
      return rc;
    });
  }, [](Napi::Env env, int64_t& value) { return Napi::BigInt::New(env, value); });
}

}  // namespace nl
