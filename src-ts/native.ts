import type {Capabilities, ClusterStats, CommandResult, ImageSpec, ImageStat, ObjectEntry, ObjectStat, OmapPage, RbdPoolStats, SnapshotInfo, Versions} from "./types";

export interface NativeCluster {
  closed: boolean; readonly instanceId: bigint;
  configReadFile(path: string | null): void; configParseEnv(variable: string | null): void;
  configGet(key: string): string; configSet(key: string, value: string): void;
  connect(): Promise<void>; close(): Promise<void>; fsid(): Promise<string>; stats(): Promise<ClusterStats>;
  pools(): Promise<string[]>; poolLookup(name: string): Promise<bigint>; poolReverseLookup(id: bigint): Promise<string>;
  poolCreate(name: string): Promise<void>; poolDelete(name: string): Promise<void>; openIoContext(pool: string): NativeIoContext;
  command(kind: string, target: string, commands: string[], input: Buffer): Promise<CommandResult>;
  pingMonitor(name: string): Promise<string>; waitForLatestOsdMap(): Promise<void>; blocklistAdd(address: string, expires: number): Promise<void>;
  serviceRegister(service: string, daemon: string, metadata: Buffer): Promise<void>; serviceUpdateStatus(status: Buffer): Promise<void>;
  monitorLog(level: string, queueSize: number, callback: (entry: import("./types").MonitorLogEntry) => void): NativeMonitorLog;
}
export interface NativeIoContext {
  closed: boolean; readonly poolName: string; readonly poolId: bigint; readonly lastVersion: bigint;
  close(): Promise<void>; setNamespace(value: string): void; setLocatorKey(value: string | null): void; setReadSnapshot(value: bigint): void;
  requiredAlignment(): Promise<bigint>; requiresAlignment(): Promise<boolean>;
  write(oid: string, data: Buffer, offset: bigint): Promise<void>; writeFull(oid: string, data: Buffer): Promise<void>; append(oid: string, data: Buffer): Promise<void>;
  read(oid: string, length: bigint, offset: bigint): Promise<Buffer>; remove(oid: string): Promise<void>; truncate(oid: string, size: bigint): Promise<void>; stat(oid: string): Promise<ObjectStat>;
  setXattr(oid: string, name: string, value: Buffer): Promise<void>; getXattr(oid: string, name: string): Promise<Buffer>; removeXattr(oid: string, name: string): Promise<void>; getXattrs(oid: string): Promise<Record<string, Buffer>>;
  getOmap(oid: string, start: string, prefix: string, max: bigint): Promise<OmapPage>; setOmap(oid: string, values: Record<string, Buffer>): Promise<void>; removeOmapKeys(oid: string, keys: string[]): Promise<void>;
  openObjectList(): NativeObjectList; notify(oid: string, payload: Buffer, timeout: bigint): Promise<Buffer>; flush(): Promise<void>;
  lockExclusive(oid: string, name: string, cookie: string, description: string, duration: bigint, flags: number): Promise<void>;
  lockShared(oid: string, name: string, cookie: string, tag: string, description: string, duration: bigint, flags: number): Promise<void>;
  unlock(oid: string, name: string, cookie: string): Promise<void>; breakLock(oid: string, name: string, client: string, cookie: string): Promise<void>; rbd(): NativeRbdPool;
  watch(oid: string, timeoutSeconds: number, queueSize: number, callback: (kind: "notification" | "error", value: any) => void): NativeObjectWatch;
}
export interface NativeRbdPool {
  list(): Promise<ImageSpec[]>; create(name: string, size: bigint, features: bigint, order: number): Promise<number>; remove(name: string): Promise<void>; rename(from: string, to: string): Promise<void>;
  open(name: string, snapshot: string | null, readOnly: boolean): Promise<NativeRbdImage>; clone(parent: string, snapshot: string, child: string, features: bigint, order: number): Promise<number>;
  trashMove(name: string, delay: bigint): Promise<void>; trashRestore(id: string, name: string): Promise<void>; trashRemove(id: string, force: boolean): Promise<void>;
  namespaceList(): Promise<string[]>; namespaceCreate(name: string): Promise<void>; namespaceRemove(name: string): Promise<void>; stats(): Promise<RbdPoolStats>;
  advanced(operation: string, strings: string[], values: bigint[]): Promise<bigint>;
}
export interface NativeRbdImage {
  closed: boolean; close(): Promise<void>; stat(): Promise<ImageStat>; size(): Promise<bigint>; resize(size: bigint, shrink: boolean): Promise<void>; features(): Promise<bigint>; flags(): Promise<bigint>;
  read(offset: bigint, length: bigint, flags: number): Promise<Buffer>; write(offset: bigint, data: Buffer, flags: number): Promise<void>; discard(offset: bigint, length: bigint): Promise<void>; flush(): Promise<void>; invalidateCache(): Promise<void>;
  snapCreate(name: string): Promise<void>; snapRemove(name: string): Promise<void>; snapList(): Promise<SnapshotInfo[]>; snapSet(name: string | null): Promise<void>; snapProtect(name: string): Promise<void>; snapUnprotect(name: string): Promise<void>; snapRollback(name: string): Promise<void>; flatten(): Promise<void>;
  metadataGet(key: string): Promise<string>; metadataSet(key: string, value: string): Promise<void>; metadataRemove(key: string): Promise<void>; metadataList(start: string, max: bigint): Promise<Record<string, string>>;
  lockAcquire(mode: number): Promise<void>; lockRelease(): Promise<void>; lockBreak(mode: number, owner: string): Promise<void>; lockOwner(): Promise<boolean>;
  watchUpdates(queueSize: number, callback: (coalesced: bigint) => void): NativeImageUpdateWatch;
  advanced(operation: string, strings: string[], values: bigint[]): Promise<bigint>;
}
export interface NativeObjectList { next(maxEntries: number): Promise<{entries: ObjectEntry[]; done: boolean}>; close(): Promise<void> }
export interface NativeMonitorLog { close(): Promise<void> }
export interface NativeObjectWatch { readonly cookie: bigint; close(): Promise<void>; ack(notifyId: bigint, cookie: bigint, payload: Buffer): Promise<void> }
export interface NativeImageUpdateWatch { close(): Promise<void> }
export interface NativeBinding {
  NativeCluster: new(clusterName: string, userName: string) => NativeCluster;
  versions(): Versions; capabilities(): Capabilities;
}

// eslint-disable-next-line @typescript-eslint/no-require-imports
export const native = require("./binding") as NativeBinding;
