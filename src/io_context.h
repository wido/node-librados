#pragma once

#include "common.h"

namespace nl {

class IoContextWrap : public Napi::ObjectWrap<IoContextWrap> {
 public:
  static Napi::FunctionReference constructor;
  static void Init(Napi::Env env, Napi::Object exports);
  static Napi::Object NewInstance(Napi::Env env, std::shared_ptr<IoState> state,
                                  std::shared_ptr<ClusterState> cluster,
                                  std::string pool);

  explicit IoContextWrap(const Napi::CallbackInfo& info);
  std::shared_ptr<IoState> state() const { return state_; }
  std::shared_ptr<ClusterState> cluster() const { return cluster_; }
  const std::string& pool() const { return pool_; }

 private:
  Napi::Value Close(const Napi::CallbackInfo& info);
  Napi::Value IsClosed(const Napi::CallbackInfo& info);
  Napi::Value PoolName(const Napi::CallbackInfo& info);
  Napi::Value PoolId(const Napi::CallbackInfo& info);
  Napi::Value SetNamespace(const Napi::CallbackInfo& info);
  Napi::Value SetLocatorKey(const Napi::CallbackInfo& info);
  Napi::Value SetReadSnapshot(const Napi::CallbackInfo& info);
  Napi::Value LastVersion(const Napi::CallbackInfo& info);
  Napi::Value Alignment(const Napi::CallbackInfo& info);
  Napi::Value RequiresAlignment(const Napi::CallbackInfo& info);
  Napi::Value Write(const Napi::CallbackInfo& info);
  Napi::Value WriteFull(const Napi::CallbackInfo& info);
  Napi::Value Append(const Napi::CallbackInfo& info);
  Napi::Value Read(const Napi::CallbackInfo& info);
  Napi::Value Remove(const Napi::CallbackInfo& info);
  Napi::Value Truncate(const Napi::CallbackInfo& info);
  Napi::Value Stat(const Napi::CallbackInfo& info);
  Napi::Value SetXattr(const Napi::CallbackInfo& info);
  Napi::Value GetXattr(const Napi::CallbackInfo& info);
  Napi::Value RemoveXattr(const Napi::CallbackInfo& info);
  Napi::Value GetXattrs(const Napi::CallbackInfo& info);
  Napi::Value GetOmap(const Napi::CallbackInfo& info);
  Napi::Value SetOmap(const Napi::CallbackInfo& info);
  Napi::Value RemoveOmapKeys(const Napi::CallbackInfo& info);
  Napi::Value WriteOp(const Napi::CallbackInfo& info);
  Napi::Value ListObjects(const Napi::CallbackInfo& info);
  Napi::Value ListWatchers(const Napi::CallbackInfo& info);
  Napi::Value Notify(const Napi::CallbackInfo& info);
  Napi::Value Flush(const Napi::CallbackInfo& info);
  Napi::Value LockExclusive(const Napi::CallbackInfo& info);
  Napi::Value LockShared(const Napi::CallbackInfo& info);
  Napi::Value Unlock(const Napi::CallbackInfo& info);
  Napi::Value BreakLock(const Napi::CallbackInfo& info);
  Napi::Value Rbd(const Napi::CallbackInfo& info);
  Napi::Value Watch(const Napi::CallbackInfo& info);

  std::shared_ptr<IoState> state_;
  std::shared_ptr<ClusterState> cluster_;
  std::string pool_;
};

}  // namespace nl
