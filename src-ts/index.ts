import {CephConfirmationError} from "./errors";
import {EventEmitter} from "node:events";
import {native, type NativeCluster, type NativeImageUpdateWatch, type NativeIoContext, type NativeMonitorLog, type NativeObjectWatch, type NativeRbdImage, type NativeRbdPool} from "./native";
import type {ClusterOptions, CommandResult, CommandTarget, Confirmation, ForceConfirmation, ImageSpec, ImageStat, MonitorLogEntry, ObjectEntry, ObjectNotification, ObjectStat, OmapPage, RbdPoolStats, SnapshotInfo, WriteStep, XattrComparison} from "./types";

export * from "./errors";
export * from "./types";

export const versions = native.versions();
export const capabilities = native.capabilities();

const finalizer = new FinalizationRegistry<{close(): Promise<void>}>(resource => { void resource.close().catch(() => undefined); });

function encodeDictionary(value: Readonly<Record<string, string>>): Buffer {
  const fields: string[] = [];
  for (const [key, item] of Object.entries(value)) fields.push(key, item);
  return Buffer.from(`${fields.join("\0")}\0\0`);
}

function requireConfirmation(operation: string, expected: string, supplied: Confirmation): void {
  if (supplied?.confirm !== expected) throw new CephConfirmationError(operation, expected);
}

export class Cluster {
  readonly #native: NativeCluster;
  readonly #finalizerToken = {};

  private constructor(handle: NativeCluster) {
    this.#native = handle;
    finalizer.register(this, handle, this.#finalizerToken);
  }

  static async connect(options: ClusterOptions = {}): Promise<Cluster> {
    const handle = new native.NativeCluster(options.clusterName ?? "ceph", options.userName ?? "client.admin");
    try {
      if (options.configFile !== false) handle.configReadFile(options.configFile ?? null);
      if (options.parseEnvironment !== false) handle.configParseEnv(null);
      for (const [key, value] of Object.entries(options.config ?? {})) handle.configSet(key, value);
      await handle.connect();
      return new Cluster(handle);
    } catch (error) {
      await handle.close().catch(() => undefined);
      throw error;
    }
  }

  get closed(): boolean { return this.#native.closed; }
  get instanceId(): bigint { return this.#native.instanceId; }
  config(key: string): string { return this.#native.configGet(key); }
  fsid(): Promise<string> { return this.#native.fsid(); }
  stats() { return this.#native.stats(); }
  pools() { return this.#native.pools(); }
  poolId(name: string) { return this.#native.poolLookup(name); }
  poolName(id: bigint) { return this.#native.poolReverseLookup(id); }
  createPool(name: string) { return this.#native.poolCreate(name); }
  deletePool(name: string, confirmation: Confirmation): Promise<void> { requireConfirmation("deletePool", name, confirmation); return this.#native.poolDelete(name); }
  ioContext(pool: string): IoContext { return new IoContext(this.#native.openIoContext(pool)); }
  pingMonitor(name: string) { return this.#native.pingMonitor(name); }
  waitForLatestOsdMap() { return this.#native.waitForLatestOsdMap(); }
  blocklist(address: string, expireSeconds = 0) { return this.#native.blocklistAdd(address, expireSeconds); }
  registerService(service: string, daemon: string, metadata: Readonly<Record<string, string>>) { return this.#native.serviceRegister(service, daemon, encodeDictionary(metadata)); }
  updateServiceStatus(status: Readonly<Record<string, string>>) { return this.#native.serviceUpdateStatus(encodeDictionary(status)); }
  monitorLogs(options: {level?: "debug" | "info" | "warn" | "warning" | "err" | "error"; queueSize?: number} = {}): MonitorLogSubscription {
    let subscription!: MonitorLogSubscription;
    const pending: MonitorLogEntry[] = [];
    const handle = this.#native.monitorLog(options.level ?? "info", options.queueSize ?? 1024, entry => subscription ? subscription.dispatch(entry) : pending.push(entry));
    subscription = new MonitorLogSubscription(handle);
    for (const entry of pending) subscription.dispatch(entry);
    return subscription;
  }
  unsafeCommand(target: CommandTarget, command: object | string, input = Buffer.alloc(0)): Promise<CommandResult> {
    const targetValue = target.kind === "osd" ? String(target.id) : target.kind === "pg" ? target.id : target.name ?? "";
    return this.#native.command(target.kind, targetValue, [typeof command === "string" ? command : JSON.stringify(command)], input);
  }
  async close(): Promise<void> { finalizer.unregister(this.#finalizerToken); await this.#native.close(); }
  async [Symbol.asyncDispose](): Promise<void> { await this.close(); }
}

export class IoContext {
  readonly #native: NativeIoContext;
  readonly #finalizerToken = {};
  constructor(handle: NativeIoContext) { this.#native = handle; finalizer.register(this, handle, this.#finalizerToken); }
  get closed() { return this.#native.closed; } get poolName() { return this.#native.poolName; } get poolId() { return this.#native.poolId; } get lastVersion() { return this.#native.lastVersion; }
  setNamespace(value = "") { this.#native.setNamespace(value); return this; }
  setLocatorKey(value: string | null) { this.#native.setLocatorKey(value); return this; }
  setReadSnapshot(value: bigint) { this.#native.setReadSnapshot(value); return this; }
  requiredAlignment() { return this.#native.requiredAlignment(); } requiresAlignment() { return this.#native.requiresAlignment(); }
  write(oid: string, data: Buffer, offset = 0n) { return this.#native.write(oid, data, offset); }
  writeFull(oid: string, data: Buffer) { return this.#native.writeFull(oid, data); }
  append(oid: string, data: Buffer) { return this.#native.append(oid, data); }
  read(oid: string, length: bigint, offset = 0n): Promise<Buffer> { return this.#native.read(oid, length, offset); }
  remove(oid: string) { return this.#native.remove(oid); } truncate(oid: string, size: bigint) { return this.#native.truncate(oid, size); } stat(oid: string): Promise<ObjectStat> { return this.#native.stat(oid); }
  setXattr(oid: string, name: string, value: Buffer) { return this.#native.setXattr(oid, name, value); } getXattr(oid: string, name: string) { return this.#native.getXattr(oid, name); } removeXattr(oid: string, name: string) { return this.#native.removeXattr(oid, name); } getXattrs(oid: string) { return this.#native.getXattrs(oid); }
  getOmap(oid: string, options: {startAfter?: string; prefix?: string; maxReturn?: bigint} = {}): Promise<OmapPage> { return this.#native.getOmap(oid, options.startAfter ?? "", options.prefix ?? "", options.maxReturn ?? 1024n); }
  setOmap(oid: string, values: Record<string, Buffer>) { return this.#native.setOmap(oid, values); } removeOmapKeys(oid: string, keys: string[]) { return this.#native.removeOmapKeys(oid, keys); }
  writeOp(oid: string): WriteOperation { return new WriteOperation(steps => this.#native.writeOp(oid, steps)); }
  async *objects(): AsyncGenerator<ObjectEntry> { for (const object of await this.#native.listObjects()) yield object; }
  notify(oid: string, payload = Buffer.alloc(0), timeoutMs = 30_000n) { return this.#native.notify(oid, payload, timeoutMs); }
  watch(oid: string, options: {timeoutSeconds?: number; queueSize?: number; autoAck?: boolean} = {}): ObjectWatch {
    let subscription!: ObjectWatch;
    const pending: Array<["notification" | "error", any]> = [];
    const handle = this.#native.watch(oid, options.timeoutSeconds ?? 30, options.queueSize ?? 1024, (kind, value) => subscription ? subscription.dispatch(kind, value) : pending.push([kind, value]));
    subscription = new ObjectWatch(handle, options.autoAck ?? true);
    for (const [kind, value] of pending) subscription.dispatch(kind, value);
    return subscription;
  }
  flush() { return this.#native.flush(); }
  lockExclusive(oid: string, name: string, cookie: string, options: {description?: string; durationMs?: bigint; flags?: number} = {}) { return this.#native.lockExclusive(oid, name, cookie, options.description ?? "", options.durationMs ?? 0n, options.flags ?? 0); }
  lockShared(oid: string, name: string, cookie: string, tag: string, options: {description?: string; durationMs?: bigint; flags?: number} = {}) { return this.#native.lockShared(oid, name, cookie, tag, options.description ?? "", options.durationMs ?? 0n, options.flags ?? 0); }
  unlock(oid: string, name: string, cookie: string) { return this.#native.unlock(oid, name, cookie); } breakLock(oid: string, name: string, client: string, cookie: string) { return this.#native.breakLock(oid, name, client, cookie); }
  rbd(): RbdPool { return new RbdPool(this.#native.rbd()); }
  async close(): Promise<void> { finalizer.unregister(this.#finalizerToken); await this.#native.close(); }
  async [Symbol.asyncDispose](): Promise<void> { await this.close(); }
}

/**
 * Steps applied to one object atomically by a single OSD operation: every step takes effect, or none does.
 * A failed check fails the whole operation before anything is written: assertExists() with ENOENT,
 * assertVersion() with ERANGE, compareXattr() with ECANCELED, and create({exclusive: true}) with EEXIST
 */
export class WriteOperation {
  readonly #steps: WriteStep[] = [];
  constructor(private readonly run: (steps: WriteStep[]) => Promise<void>) {}
  assertExists() { this.#steps.push({type: "assertExists"}); return this; }
  assertVersion(version: bigint) { this.#steps.push({type: "assertVersion", version}); return this; }
  compareXattr(name: string, comparison: XattrComparison, value: Buffer) { this.#steps.push({type: "compareXattr", name, comparison, value}); return this; }
  create(options: {exclusive?: boolean} = {}) { this.#steps.push({type: "create", exclusive: options.exclusive ?? true}); return this; }
  write(data: Buffer, offset = 0n) { this.#steps.push({type: "write", data, offset}); return this; }
  writeFull(data: Buffer) { this.#steps.push({type: "writeFull", data}); return this; }
  append(data: Buffer) { this.#steps.push({type: "append", data}); return this; }
  truncate(size: bigint) { this.#steps.push({type: "truncate", size}); return this; }
  zero(offset: bigint, length: bigint) { this.#steps.push({type: "zero", offset, length}); return this; }
  remove() { this.#steps.push({type: "remove"}); return this; }
  setXattr(name: string, value: Buffer) { this.#steps.push({type: "setXattr", name, value}); return this; }
  removeXattr(name: string) { this.#steps.push({type: "removeXattr", name}); return this; }
  setOmap(values: Record<string, Buffer>) { this.#steps.push({type: "setOmap", values}); return this; }
  removeOmapKeys(keys: string[]) { this.#steps.push({type: "removeOmapKeys", keys}); return this; }
  operate(): Promise<void> { return this.run([...this.#steps]); }
}

export class RbdPool {
  constructor(readonly nativeHandle: NativeRbdPool) {}
  list(): Promise<ImageSpec[]> { return this.nativeHandle.list(); }
  create(name: string, size: bigint, options: {features?: bigint; order?: number} = {}) { return this.nativeHandle.create(name, size, options.features ?? 0n, options.order ?? 0); }
  remove(name: string, confirmation: Confirmation) { requireConfirmation("RbdPool.remove", name, confirmation); return this.nativeHandle.remove(name); }
  rename(from: string, to: string) { return this.nativeHandle.rename(from, to); }
  async open(name: string, options: {snapshot?: string; readOnly?: boolean} = {}): Promise<RbdImage> { return new RbdImage(await this.nativeHandle.open(name, options.snapshot ?? null, options.readOnly ?? false), name); }
  clone(parent: string, snapshot: string, child: string, options: {features?: bigint; order?: number} = {}) { return this.nativeHandle.clone(parent, snapshot, child, options.features ?? 0n, options.order ?? 0); }
  moveToTrash(name: string, delaySeconds = 0n) { return this.nativeHandle.trashMove(name, delaySeconds); }
  restoreFromTrash(id: string, name: string) { return this.nativeHandle.trashRestore(id, name); }
  removeFromTrash(id: string, options: ForceConfirmation) { requireConfirmation("RbdPool.removeFromTrash", id, options); return this.nativeHandle.trashRemove(id, options.force ?? false); }
  namespaces() { return this.nativeHandle.namespaceList(); } createNamespace(name: string) { return this.nativeHandle.namespaceCreate(name); } removeNamespace(name: string, confirmation: Confirmation) { requireConfirmation("RbdPool.removeNamespace", name, confirmation); return this.nativeHandle.namespaceRemove(name); }
  stats(): Promise<RbdPoolStats> { return this.nativeHandle.stats(); }
  mirrorMode(): Promise<bigint> { return this.nativeHandle.advanced("mirrorModeGet", [], []); }
  async setMirrorMode(mode: number): Promise<void> { await this.nativeHandle.advanced("mirrorModeSet", [], [BigInt(mode)]); }
  async prepareMigration(source: string, destination: string): Promise<void> { await this.nativeHandle.advanced("migrationPrepare", [source, destination], []); }
  async executeMigration(name: string): Promise<void> { await this.nativeHandle.advanced("migrationExecute", [name], []); }
  async commitMigration(name: string, confirmation: Confirmation): Promise<void> { requireConfirmation("RbdPool.commitMigration", name, confirmation); await this.nativeHandle.advanced("migrationCommit", [name], []); }
  async abortMigration(name: string, confirmation: Confirmation): Promise<void> { requireConfirmation("RbdPool.abortMigration", name, confirmation); await this.nativeHandle.advanced("migrationAbort", [name], []); }
  async createGroup(name: string): Promise<void> { await this.nativeHandle.advanced("groupCreate", [name], []); }
  async removeGroup(name: string, confirmation: Confirmation): Promise<void> { requireConfirmation("RbdPool.removeGroup", name, confirmation); await this.nativeHandle.advanced("groupRemove", [name], []); }
  async renameGroup(from: string, to: string): Promise<void> { await this.nativeHandle.advanced("groupRename", [from, to], []); }
  async addImageToGroup(group: string, image: string): Promise<void> { await this.nativeHandle.advanced("groupImageAdd", [group, image], []); }
  async removeImageFromGroup(group: string, image: string): Promise<void> { await this.nativeHandle.advanced("groupImageRemove", [group, image], []); }
  async createGroupSnapshot(group: string, snapshot: string): Promise<void> { await this.nativeHandle.advanced("groupSnapshotCreate", [group, snapshot], []); }
  async removeGroupSnapshot(group: string, snapshot: string, confirmation: Confirmation): Promise<void> { requireConfirmation("RbdPool.removeGroupSnapshot", `${group}@${snapshot}`, confirmation); await this.nativeHandle.advanced("groupSnapshotRemove", [group, snapshot], []); }
  async rollbackGroupSnapshot(group: string, snapshot: string, confirmation: Confirmation): Promise<void> { requireConfirmation("RbdPool.rollbackGroupSnapshot", `${group}@${snapshot}`, confirmation); await this.nativeHandle.advanced("groupSnapshotRollback", [group, snapshot], []); }
}

export class RbdImage {
  readonly #finalizerToken = {};
  constructor(readonly nativeHandle: NativeRbdImage, readonly name: string) { finalizer.register(this, nativeHandle, this.#finalizerToken); }
  get closed() { return this.nativeHandle.closed; }
  stat(): Promise<ImageStat> { return this.nativeHandle.stat(); } size() { return this.nativeHandle.size(); } resize(size: bigint, options: {allowShrink?: boolean} = {}) { return this.nativeHandle.resize(size, options.allowShrink ?? false); } features() { return this.nativeHandle.features(); } flags() { return this.nativeHandle.flags(); }
  read(offset: bigint, length: bigint, flags = 0) { return this.nativeHandle.read(offset, length, flags); } write(offset: bigint, data: Buffer, flags = 0) { return this.nativeHandle.write(offset, data, flags); } discard(offset: bigint, length: bigint) { return this.nativeHandle.discard(offset, length); } flush() { return this.nativeHandle.flush(); } invalidateCache() { return this.nativeHandle.invalidateCache(); }
  snapshots() { return this.nativeHandle.snapList(); } createSnapshot(name: string) { return this.nativeHandle.snapCreate(name); } removeSnapshot(name: string, confirmation: Confirmation) { requireConfirmation("RbdImage.removeSnapshot", name, confirmation); return this.nativeHandle.snapRemove(name); } selectSnapshot(name: string | null) { return this.nativeHandle.snapSet(name); } protectSnapshot(name: string) { return this.nativeHandle.snapProtect(name); } unprotectSnapshot(name: string) { return this.nativeHandle.snapUnprotect(name); } rollbackSnapshot(name: string, confirmation: Confirmation) { requireConfirmation("RbdImage.rollbackSnapshot", name, confirmation); return this.nativeHandle.snapRollback(name); } flatten() { return this.nativeHandle.flatten(); }
  metadata(key: string) { return this.nativeHandle.metadataGet(key); } setMetadata(key: string, value: string) { return this.nativeHandle.metadataSet(key, value); } removeMetadata(key: string) { return this.nativeHandle.metadataRemove(key); } listMetadata(startAfter = "", maxReturn = 1024n) { return this.nativeHandle.metadataList(startAfter, maxReturn); }
  acquireLock(mode = 0) { return this.nativeHandle.lockAcquire(mode); } releaseLock() { return this.nativeHandle.lockRelease(); } breakLock(mode: number, owner: string) { return this.nativeHandle.lockBreak(mode, owner); } isLockOwner() { return this.nativeHandle.lockOwner(); }
  watchUpdates(options: {queueSize?: number} = {}): ImageUpdateWatch {
    let subscription!: ImageUpdateWatch;
    const pending: bigint[] = [];
    const handle = this.nativeHandle.watchUpdates(options.queueSize ?? 1024, count => subscription ? subscription.dispatch(count) : pending.push(count));
    subscription = new ImageUpdateWatch(handle);
    for (const count of pending) subscription.dispatch(count);
    return subscription;
  }
  async enableMirroring(mode: number): Promise<void> { await this.nativeHandle.advanced("mirrorEnable", [], [BigInt(mode)]); }
  async disableMirroring(force = false): Promise<void> { await this.nativeHandle.advanced("mirrorDisable", [], [force ? 1n : 0n]); }
  async promoteMirror(force = false): Promise<void> { await this.nativeHandle.advanced("mirrorPromote", [], [force ? 1n : 0n]); }
  async demoteMirror(): Promise<void> { await this.nativeHandle.advanced("mirrorDemote", [], []); }
  async resyncMirror(): Promise<void> { await this.nativeHandle.advanced("mirrorResync", [], []); }
  mirrorSnapshot(): Promise<bigint> { return this.nativeHandle.advanced("mirrorSnapshot", [], []); }
  async sparsify(sparseSize: bigint): Promise<void> { await this.nativeHandle.advanced("sparsify", [], [sparseSize]); }
  async rebuildObjectMap(): Promise<void> { await this.nativeHandle.advanced("rebuildObjectMap", [], []); }
  async updateFeatures(features: bigint, enabled: boolean): Promise<void> { await this.nativeHandle.advanced("updateFeatures", [], [features, enabled ? 1n : 0n]); }
  async formatEncryption(format: number, algorithm: number, passphrase: string): Promise<void> { await this.nativeHandle.advanced("encryptionFormat", [passphrase], [BigInt(format), BigInt(algorithm)]); }
  async loadEncryption(format: number, algorithm: number, passphrase: string): Promise<void> { await this.nativeHandle.advanced("encryptionLoad", [passphrase], [BigInt(format), BigInt(algorithm)]); }
  async close(): Promise<void> { finalizer.unregister(this.#finalizerToken); await this.nativeHandle.close(); }
  async [Symbol.asyncDispose](): Promise<void> { await this.close(); }
}

export class MonitorLogSubscription extends EventEmitter {
  #closed = false;
  constructor(private readonly handle: NativeMonitorLog) { super(); }
  dispatch(entry: MonitorLogEntry): void {
    if (this.#closed) return;
    if (entry.dropped > 0n) this.emit("overflow", entry.dropped);
    this.emit("log", entry);
  }
  async close(): Promise<void> { if (this.#closed) return; this.#closed = true; await this.handle.close(); this.emit("close"); }
  async [Symbol.asyncDispose](): Promise<void> { await this.close(); }
}

export class ObjectWatch extends EventEmitter {
  #closed = false;
  constructor(private readonly handle: NativeObjectWatch, readonly autoAck: boolean) { super(); }
  get cookie(): bigint { return this.handle.cookie; }
  dispatch(kind: "notification" | "error", value: any): void {
    if (this.#closed) return;
    if (kind === "error") {
      this.emit("error", new Error(`RADOS watch failed with code ${value.code}`));
      return;
    }
    let acknowledged = false;
    const notification: ObjectNotification = {
      ...value,
      ack: async (payload = Buffer.alloc(0)) => {
        if (acknowledged) throw new Error("Notification was already acknowledged");
        acknowledged = true;
        await this.handle.ack(value.notifyId, value.cookie, payload);
      },
    };
    if (notification.dropped > 0n) this.emit("overflow", notification.dropped);
    this.emit("notification", notification);
    if (this.autoAck && !acknowledged) void notification.ack().catch(error => this.emit("error", error));
  }
  async close(): Promise<void> { if (this.#closed) return; this.#closed = true; await this.handle.close(); this.emit("close"); }
  async [Symbol.asyncDispose](): Promise<void> { await this.close(); }
}

export class ImageUpdateWatch extends EventEmitter {
  #closed = false;
  constructor(private readonly handle: NativeImageUpdateWatch) { super(); }
  dispatch(coalesced: bigint): void { if (!this.#closed) this.emit("update", {coalesced}); }
  async close(): Promise<void> { if (this.#closed) return; this.#closed = true; await this.handle.close(); this.emit("close"); }
  async [Symbol.asyncDispose](): Promise<void> { await this.close(); }
}
