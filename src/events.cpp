#include "events.h"

namespace nl {

Napi::FunctionReference ObjectWatchWrap::constructor;
Napi::FunctionReference MonitorLogWrap::constructor;
Napi::FunctionReference ImageUpdateWatchWrap::constructor;

struct ObjectNotification {
  uint64_t notify_id{};
  uint64_t cookie{};
  uint64_t notifier_id{};
  std::vector<uint8_t> payload;
  uint64_t dropped{};
  int error{};
};

struct ObjectWatchWrap::Data {
  std::shared_ptr<IoState> io;
  std::shared_ptr<ClusterState> cluster;
  std::string oid;
  uint64_t cookie{};
  std::atomic<uint64_t> dropped{0};
  std::atomic<bool> closed{false};
  Napi::ThreadSafeFunction callback;

  int close() {
    if (closed.exchange(true)) return 0;
    int result = io->use([&](rados_ioctx_t handle) { return rados_unwatch2(handle, cookie); });
    int flush_result = cluster->use([](rados_t handle) { return rados_watch_flush(handle); });
    callback.Release();
    io->remove_child();
    io->end_op();
    return result < 0 ? result : flush_result;
  }
};

static void object_watch_cb(void* arg, uint64_t notify_id, uint64_t cookie,
                            uint64_t notifier_id, void* payload, size_t length) {
  auto* data = static_cast<ObjectWatchWrap::Data*>(arg);
  if (!data || data->closed.load()) return;
  auto* event = new ObjectNotification{
      notify_id, cookie, notifier_id,
      std::vector<uint8_t>(static_cast<uint8_t*>(payload), static_cast<uint8_t*>(payload) + length),
      data->dropped.exchange(0), 0};
  napi_status status = data->callback.NonBlockingCall(
      event, [](Napi::Env env, Napi::Function callback, ObjectNotification* value) {
        auto out = Napi::Object::New(env);
        out.Set("notifyId", Napi::BigInt::New(env, value->notify_id));
        out.Set("cookie", Napi::BigInt::New(env, value->cookie));
        out.Set("notifierId", Napi::BigInt::New(env, value->notifier_id));
        out.Set("payload", Napi::Buffer<uint8_t>::Copy(env, value->payload.data(), value->payload.size()));
        out.Set("dropped", Napi::BigInt::New(env, value->dropped));
        callback.Call({Napi::String::New(env, "notification"), out});
        delete value;
      });
  if (status != napi_ok) {
    data->dropped.fetch_add(event->dropped + 1);
    delete event;
    data->io->use([&](rados_ioctx_t handle) {
      return rados_notify_ack(handle, data->oid.c_str(), notify_id, cookie, nullptr, 0);
    });
  }
}

static void object_watch_error_cb(void* arg, uint64_t cookie, int error) {
  auto* data = static_cast<ObjectWatchWrap::Data*>(arg);
  if (!data || data->closed.load()) return;
  auto* event = new ObjectNotification{};
  event->cookie = cookie;
  event->error = error;
  napi_status status = data->callback.NonBlockingCall(
      event, [](Napi::Env env, Napi::Function callback, ObjectNotification* value) {
        auto out = Napi::Object::New(env);
        out.Set("cookie", Napi::BigInt::New(env, value->cookie));
        out.Set("code", value->error);
        callback.Call({Napi::String::New(env, "error"), out});
        delete value;
      });
  if (status != napi_ok) delete event;
}

void ObjectWatchWrap::Init(Napi::Env env, Napi::Object exports) {
  auto function = DefineClass(env, "NativeObjectWatch", {
      InstanceMethod("close", &ObjectWatchWrap::Close),
      InstanceMethod("ack", &ObjectWatchWrap::Ack),
      InstanceAccessor("cookie", &ObjectWatchWrap::Cookie, nullptr),
  });
  constructor = Napi::Persistent(function);
  constructor.SuppressDestruct();
  exports.Set("NativeObjectWatch", function);
}

Napi::Object ObjectWatchWrap::Create(Napi::Env env, std::shared_ptr<IoState> io,
                                     std::shared_ptr<ClusterState> cluster,
                                     std::string oid, uint32_t timeout,
                                     uint32_t queue_size, Napi::Function callback) {
  if (queue_size == 0) throw Napi::RangeError::New(env, "queueSize must be greater than zero");
  auto data = std::make_shared<Data>();
  data->io = std::move(io);
  data->cluster = std::move(cluster);
  data->oid = std::move(oid);
  data->callback = Napi::ThreadSafeFunction::New(env, callback, "RadosObjectWatch", queue_size, 1);
  data->io->add_child();
  // acks go out in the io context's namespace, which may not change while the watch is registered
  data->io->begin_op();
  int result = data->io->use([&](rados_ioctx_t handle) {
    return rados_watch3(handle, data->oid.c_str(), &data->cookie, object_watch_cb,
                        object_watch_error_cb, timeout, data.get());
  });
  if (result < 0) {
    data->closed = true;
    data->callback.Release();
    data->io->remove_child();
    data->io->end_op();
    throw_ceph(env, result, "rados_watch3", data->oid);
    return Napi::Object::New(env);
  }
  auto* holder = new std::shared_ptr<Data>(std::move(data));
  return constructor.New({Napi::External<std::shared_ptr<Data>>::New(env, holder)});
}

ObjectWatchWrap::ObjectWatchWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<ObjectWatchWrap>(info) {
  if (!info.Length() || !info[0].IsExternal())
    throw Napi::TypeError::New(info.Env(), "ObjectWatch cannot be constructed directly");
  auto* holder = info[0].As<Napi::External<std::shared_ptr<Data>>>().Data();
  data_ = std::move(*holder);
  delete holder;
}

ObjectWatchWrap::~ObjectWatchWrap() {
  if (data_ && !data_->closed.load()) data_->close();
}

Napi::Value ObjectWatchWrap::Close(const Napi::CallbackInfo& info) {
  auto data = data_;
  return async_void(info.Env(), "rados_unwatch2", data->oid,
                    [data] { return data->close(); });
}

Napi::Value ObjectWatchWrap::Ack(const Napi::CallbackInfo& info) {
  uint64_t notify = uint64_arg(info[0], "notifyId");
  uint64_t cookie = uint64_arg(info[1], "cookie");
  std::vector<uint8_t> payload;
  if (info.Length() > 2 && info[2].IsBuffer()) {
    auto value = info[2].As<Napi::Buffer<uint8_t>>();
    payload.assign(value.Data(), value.Data() + value.Length());
  }
  auto data = data_;
  return async_void(info.Env(), "rados_notify_ack", data->oid,
      [data, notify, cookie, payload] {
        return data->io->use([&](rados_ioctx_t handle) {
          return rados_notify_ack(handle, data->oid.c_str(), notify, cookie,
                                  reinterpret_cast<const char*>(payload.data()),
                                  static_cast<int>(payload.size()));
        });
      });
}

Napi::Value ObjectWatchWrap::Cookie(const Napi::CallbackInfo& info) {
  return Napi::BigInt::New(info.Env(), data_->cookie);
}

struct LogEvent {
  std::string line, channel, who, name, level, message;
  uint64_t seconds{}, nanoseconds{}, sequence{}, dropped{};
};

struct MonitorLogWrap::Data {
  std::shared_ptr<ClusterState> cluster;
  std::atomic<uint64_t> dropped{0};
  std::atomic<bool> closed{false};
  Napi::ThreadSafeFunction callback;

  int close() {
    if (closed.exchange(true)) return 0;
    int result = cluster->use([](rados_t handle) {
      return rados_monitor_log2(handle, "info", nullptr, nullptr);
    });
    callback.Release();
    cluster->remove_child();
    return result;
  }
};

static void monitor_log_cb(void* arg, const char* line, const char* channel,
                           const char* who, const char* name, uint64_t sec,
                           uint64_t nsec, uint64_t seq, const char* level,
                           const char* message) {
  auto* data = static_cast<MonitorLogWrap::Data*>(arg);
  if (!data || data->closed.load()) return;
  auto* event = new LogEvent{
      line ? line : "", channel ? channel : "", who ? who : "", name ? name : "",
      level ? level : "", message ? message : "", sec, nsec, seq,
      data->dropped.exchange(0)};
  napi_status status = data->callback.NonBlockingCall(
      event, [](Napi::Env env, Napi::Function callback, LogEvent* value) {
        auto out = Napi::Object::New(env);
        out.Set("line", value->line); out.Set("channel", value->channel);
        out.Set("who", value->who); out.Set("name", value->name);
        out.Set("level", value->level); out.Set("message", value->message);
        out.Set("seconds", Napi::BigInt::New(env, value->seconds));
        out.Set("nanoseconds", Napi::BigInt::New(env, value->nanoseconds));
        out.Set("sequence", Napi::BigInt::New(env, value->sequence));
        out.Set("dropped", Napi::BigInt::New(env, value->dropped));
        callback.Call({out});
        delete value;
      });
  if (status != napi_ok) {
    data->dropped.fetch_add(event->dropped + 1);
    delete event;
  }
}

void MonitorLogWrap::Init(Napi::Env env, Napi::Object exports) {
  auto function = DefineClass(env, "NativeMonitorLog", {
      InstanceMethod("close", &MonitorLogWrap::Close),
  });
  constructor = Napi::Persistent(function);
  constructor.SuppressDestruct();
  exports.Set("NativeMonitorLog", function);
}

Napi::Object MonitorLogWrap::Create(Napi::Env env,
                                    std::shared_ptr<ClusterState> cluster,
                                    std::string level, uint32_t queue_size,
                                    Napi::Function callback) {
  if (queue_size == 0) throw Napi::RangeError::New(env, "queueSize must be greater than zero");
  auto data = std::make_shared<Data>();
  data->cluster = std::move(cluster);
  data->callback = Napi::ThreadSafeFunction::New(env, callback, "RadosMonitorLog", queue_size, 1);
  data->cluster->add_child();
  int result = data->cluster->use([&](rados_t handle) {
    return rados_monitor_log2(handle, level.c_str(), monitor_log_cb, data.get());
  });
  if (result < 0) {
    data->closed = true;
    data->callback.Release();
    data->cluster->remove_child();
    throw_ceph(env, result, "rados_monitor_log2", level);
    return Napi::Object::New(env);
  }
  auto* holder = new std::shared_ptr<Data>(std::move(data));
  return constructor.New({Napi::External<std::shared_ptr<Data>>::New(env, holder)});
}

MonitorLogWrap::MonitorLogWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<MonitorLogWrap>(info) {
  if (!info.Length() || !info[0].IsExternal())
    throw Napi::TypeError::New(info.Env(), "MonitorLog cannot be constructed directly");
  auto* holder = info[0].As<Napi::External<std::shared_ptr<Data>>>().Data();
  data_ = std::move(*holder);
  delete holder;
}

MonitorLogWrap::~MonitorLogWrap() {
  if (data_ && !data_->closed.load()) data_->close();
}

Napi::Value MonitorLogWrap::Close(const Napi::CallbackInfo& info) {
  auto data = data_;
  return async_void(info.Env(), "rados_monitor_log2", {},
                    [data] { return data->close(); });
}

struct ImageUpdateWatchWrap::Data {
  std::shared_ptr<ImageState> image;
  uint64_t handle{};
  std::atomic<uint64_t> coalesced{0};
  std::atomic<bool> closed{false};
  Napi::ThreadSafeFunction callback;

  int close() {
    if (closed.exchange(true)) return 0;
    int result = image->use([&](rbd_image_t value) {
      return rbd_update_unwatch(value, handle);
    });
    callback.Release();
    image->remove_child();
    return result;
  }
};

static void image_update_cb(void* arg) {
  auto* data = static_cast<ImageUpdateWatchWrap::Data*>(arg);
  if (!data || data->closed.load()) return;
  auto* count = new uint64_t(data->coalesced.exchange(0) + 1);
  napi_status status = data->callback.NonBlockingCall(
      count, [](Napi::Env env, Napi::Function callback, uint64_t* value) {
        callback.Call({Napi::BigInt::New(env, *value)});
        delete value;
      });
  if (status != napi_ok) {
    data->coalesced.fetch_add(*count);
    delete count;
  }
}

void ImageUpdateWatchWrap::Init(Napi::Env env, Napi::Object exports) {
  auto function = DefineClass(env, "NativeImageUpdateWatch", {
      InstanceMethod("close", &ImageUpdateWatchWrap::Close),
  });
  constructor = Napi::Persistent(function);
  constructor.SuppressDestruct();
  exports.Set("NativeImageUpdateWatch", function);
}

Napi::Object ImageUpdateWatchWrap::Create(Napi::Env env,
                                          std::shared_ptr<ImageState> image,
                                          uint32_t queue_size,
                                          Napi::Function callback) {
  if (queue_size == 0) throw Napi::RangeError::New(env, "queueSize must be greater than zero");
  auto data = std::make_shared<Data>();
  data->image = std::move(image);
  data->callback = Napi::ThreadSafeFunction::New(env, callback, "RbdImageUpdateWatch", queue_size, 1);
  data->image->add_child();
  int result = data->image->use([&](rbd_image_t value) {
    return rbd_update_watch(value, &data->handle, image_update_cb, data.get());
  });
  if (result < 0) {
    data->closed = true;
    data->callback.Release();
    data->image->remove_child();
    throw_ceph(env, result, "rbd_update_watch");
    return Napi::Object::New(env);
  }
  auto* holder = new std::shared_ptr<Data>(std::move(data));
  return constructor.New({Napi::External<std::shared_ptr<Data>>::New(env, holder)});
}

ImageUpdateWatchWrap::ImageUpdateWatchWrap(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<ImageUpdateWatchWrap>(info) {
  if (!info.Length() || !info[0].IsExternal())
    throw Napi::TypeError::New(info.Env(), "ImageUpdateWatch cannot be constructed directly");
  auto* holder = info[0].As<Napi::External<std::shared_ptr<Data>>>().Data();
  data_ = std::move(*holder);
  delete holder;
}

ImageUpdateWatchWrap::~ImageUpdateWatchWrap() {
  if (data_ && !data_->closed.load()) data_->close();
}

Napi::Value ImageUpdateWatchWrap::Close(const Napi::CallbackInfo& info) {
  auto data = data_;
  return async_void(info.Env(), "rbd_update_unwatch", {},
                    [data] { return data->close(); });
}

}  // namespace nl
