#pragma once

#include <napi.h>
#include <rados/librados.h>
#include <rbd/librbd.h>

#include <cerrno>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace nl {

template <typename Handle>
class HandleState {
 public:
  using Close = std::function<int(Handle)>;

  HandleState(Handle handle, Close close) : handle_(handle), close_(std::move(close)) {}
  ~HandleState() { close_now(); }

  HandleState(const HandleState&) = delete;
  HandleState& operator=(const HandleState&) = delete;

  template <typename Fn>
  auto use(Fn&& fn) const -> decltype(fn(std::declval<Handle>())) {
    std::shared_lock lock(mutex_);
    if (closed_ || !handle_) throw std::runtime_error("resource is closed");
    return fn(handle_);
  }

  int close_now() {
    std::unique_lock lock(mutex_);
    if (closed_) return 0;
    closed_ = true;
    Handle handle = handle_;
    handle_ = nullptr;
    return handle ? close_(handle) : 0;
  }

  bool closed() const {
    std::shared_lock lock(mutex_);
    return closed_;
  }

  void add_child() { children_.fetch_add(1); }
  void remove_child() { children_.fetch_sub(1); }
  int close_checked() {
    if (children_.load() != 0) return -EBUSY;
    return close_now();
  }

 private:
  mutable std::shared_mutex mutex_;
  Handle handle_{};
  Close close_;
  bool closed_{false};
  std::atomic<uint32_t> children_{0};
};

// The bytes of a Buffer argument, for an operation on the thread pool, without copying them. The Buffer is
// referenced until the operation's worker is deleted on the main thread. As with fs.write(), the caller must
// not change or transfer the Buffer before the operation settles
class BufferView {
 public:
  BufferView(const Napi::CallbackInfo& info, size_t index, const char* name) {
    if (info.Length() <= index || !info[index].IsBuffer())
      throw Napi::TypeError::New(info.Env(), std::string(name) + " must be a Buffer");
    auto buffer = info[index].As<Napi::Buffer<uint8_t>>();
    data_ = reinterpret_cast<const char*>(buffer.Data());
    size_ = buffer.Length();
    ref_ = std::make_shared<Napi::Reference<Napi::Buffer<uint8_t>>>(Napi::Persistent(buffer));
  }
  const char* data() const { return data_; }
  size_t size() const { return size_; }

 private:
  const char* data_{};
  size_t size_{};
  std::shared_ptr<Napi::Reference<Napi::Buffer<uint8_t>>> ref_;
};

// Bytes read on the thread pool into memory that becomes the Buffer handed to JavaScript, instead of being
// copied into one on the main thread
struct ReadBytes {
  std::unique_ptr<uint8_t[]> data;
  size_t capacity{};
  size_t size{};

  char* allocate(size_t length) {
    data.reset(new uint8_t[length ? length : 1]);
    capacity = length;
    size = 0;
    return reinterpret_cast<char*>(data.get());
  }

  Napi::Value to_buffer(Napi::Env env) {
    // a short read would keep the whole allocation alive with the Buffer, so it is copied instead
    if (size < capacity / 2) return Napi::Buffer<uint8_t>::Copy(env, data.get(), size);
    const int64_t external = static_cast<int64_t>(capacity);
    auto buffer = Napi::Buffer<uint8_t>::NewOrCopy(env, data.release(), size, [external](Napi::BasicEnv env, uint8_t* bytes) {
      delete[] bytes;
      Napi::MemoryManagement::AdjustExternalMemory(env, -external);
    });
    // the garbage collector does not see memory it did not allocate unless it is told
    Napi::MemoryManagement::AdjustExternalMemory(env, external);
    return buffer;
  }
};

using ClusterState = HandleState<rados_t>;
using IoState = HandleState<rados_ioctx_t>;
using ImageState = HandleState<rbd_image_t>;

Napi::Error ceph_error(Napi::Env env, int code, const std::string& operation,
                       const std::string& resource = {});
void throw_ceph(Napi::Env env, int code, const std::string& operation,
                const std::string& resource = {});
uint64_t uint64_arg(const Napi::Value& value, const char* name);
int64_t int64_arg(const Napi::Value& value, const char* name);
std::string string_arg(const Napi::CallbackInfo& info, size_t index, const char* name);
Napi::Object version_object(Napi::Env env, int major, int minor, int extra);

template <typename Result>
class Worker final : public Napi::AsyncWorker {
 public:
  using Work = std::function<int(Result&)>;
  using Convert = std::function<Napi::Value(Napi::Env, Result&)>;

  Worker(Napi::Env env, std::string operation, std::string resource,
         Work work, Convert convert)
      : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)),
        operation_(std::move(operation)), resource_(std::move(resource)),
        work_(std::move(work)), convert_(std::move(convert)) {}

  void Execute() override {
    try {
      code_ = work_(result_);
    } catch (const std::exception& error) {
      SetError(error.what());
    } catch (...) {
      SetError("unknown native exception");
    }
  }

  void OnOK() override {
    if (code_ < 0) {
      deferred_.Reject(ceph_error(Env(), code_, operation_, resource_).Value());
      return;
    }
    try {
      deferred_.Resolve(convert_(Env(), result_));
    } catch (const Napi::Error& error) {
      deferred_.Reject(error.Value());
    }
  }

  void OnError(const Napi::Error& error) override { deferred_.Reject(error.Value()); }
  Napi::Promise QueueAndGetPromise() { Queue(); return deferred_.Promise(); }

 private:
  Napi::Promise::Deferred deferred_;
  std::string operation_;
  std::string resource_;
  Work work_;
  Convert convert_;
  Result result_{};
  int code_{0};
};

struct VoidResult {};

inline Napi::Promise async_void(Napi::Env env, std::string operation,
                                std::string resource, std::function<int()> work) {
  auto* worker = new Worker<VoidResult>(
      env, std::move(operation), std::move(resource),
      [work = std::move(work)](VoidResult&) { return work(); },
      [](Napi::Env env, VoidResult&) { return env.Undefined(); });
  return worker->QueueAndGetPromise();
}

template <typename T>
inline Napi::Promise async_value(Napi::Env env, std::string operation,
                                 std::string resource,
                                 std::function<int(T&)> work,
                                 std::function<Napi::Value(Napi::Env, T&)> convert) {
  auto* worker = new Worker<T>(env, std::move(operation), std::move(resource),
                               std::move(work), std::move(convert));
  return worker->QueueAndGetPromise();
}

}  // namespace nl
