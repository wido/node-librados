export interface CephVersion {
  major: number;
  minor: number;
  extra: number;
  string: string;
}

export interface Versions {
  rados: CephVersion;
  rbd: CephVersion;
  napi: number;
}

export interface Capabilities {
  radosWatch: boolean;
  radosMonitorLog: boolean;
  radosService: boolean;
  radosBlocklist: boolean;
  rbdNamespace: boolean;
  rbdTrash: boolean;
  rbdMigration: boolean;
  rbdEncryption: boolean;
  rbdGroups: boolean;
  rbdMirroring: boolean;
  rbdUpdateWatch: boolean;
  rbdQuiesceWatch: boolean;
}

export interface ClusterOptions {
  clusterName?: string;
  userName?: string;
  /** `false` skips config-file loading; null/undefined searches Ceph's standard paths. */
  configFile?: string | null | false;
  parseEnvironment?: boolean;
  config?: Readonly<Record<string, string>>;
}

export interface ClusterStats {
  kb: bigint;
  kbUsed: bigint;
  kbAvailable: bigint;
  objects: bigint;
}

export interface ObjectStat {
  size: bigint;
  modifiedAt: Date;
}

export interface ObjectEntry {
  name: string;
  locator: string;
  namespace: string;
}

export type XattrComparison = "eq" | "ne" | "gt" | "gte" | "lt" | "lte";

export type WriteStep =
  | {type: "assertExists"}
  | {type: "assertVersion"; version: bigint}
  | {type: "compareXattr"; name: string; comparison: XattrComparison; value: Buffer}
  | {type: "create"; exclusive: boolean}
  | {type: "write"; data: Buffer; offset: bigint}
  | {type: "writeFull"; data: Buffer}
  | {type: "append"; data: Buffer}
  | {type: "truncate"; size: bigint}
  | {type: "zero"; offset: bigint; length: bigint}
  | {type: "remove"}
  | {type: "setXattr"; name: string; value: Buffer}
  | {type: "removeXattr"; name: string}
  | {type: "setOmap"; values: Record<string, Buffer>}
  | {type: "removeOmapKeys"; keys: string[]};

export interface OmapPage {
  entries: Record<string, Buffer>;
  more: boolean;
}

export type CommandTarget =
  | {kind: "mon"; name?: string}
  | {kind: "mgr"; name?: string}
  | {kind: "osd"; id: number}
  | {kind: "pg"; id: string};

export interface CommandResult {
  output: Buffer;
  status: string;
}

export interface ImageSpec { id: string; name: string }
export interface ImageStat { size: bigint; objectSize: bigint; objects: bigint; order: number }
export interface SnapshotInfo { id: bigint; size: bigint; name: string }
export interface RbdPoolStats {
  images: bigint;
  imageProvisionedBytes: bigint;
  imageMaxProvisionedBytes: bigint;
  imageSnapshots: bigint;
  trashImages: bigint;
  trashProvisionedBytes: bigint;
  trashMaxProvisionedBytes: bigint;
  trashSnapshots: bigint;
}

export interface Confirmation { confirm: string }
export interface ForceConfirmation extends Confirmation { force?: boolean }

export interface MonitorLogEntry {
  line: string; channel: string; who: string; name: string; level: string; message: string;
  seconds: bigint; nanoseconds: bigint; sequence: bigint; dropped: bigint;
}

export interface ObjectNotification {
  notifyId: bigint; cookie: bigint; notifierId: bigint; payload: Buffer; dropped: bigint;
  ack(payload?: Buffer): Promise<void>;
}

export enum RbdLockMode { Exclusive = 0, Shared = 1 }
export enum RbdMirrorMode { Disabled = 0, Image = 1, Pool = 2 }
export enum RbdMirrorImageMode { Journal = 0, Snapshot = 1 }
export enum RbdEncryptionFormat { Luks1 = 0, Luks2 = 1 }
export enum RbdEncryptionAlgorithm { Aes128 = 0, Aes256 = 1 }

export const RbdFeature = {
  Layering: 1n,
  StripingV2: 2n,
  ExclusiveLock: 4n,
  ObjectMap: 8n,
  FastDiff: 16n,
  DeepFlatten: 32n,
  Journaling: 64n,
  DataPool: 128n,
  Operations: 256n,
  Migrating: 512n,
  NonPrimary: 1024n,
} as const;

export const RadosOperationFlag = {
  Exclusive: 0x1,
  FailOk: 0x2,
  Random: 0x4,
  Sequential: 0x8,
  WillNeed: 0x10,
  DontNeed: 0x20,
  NoCache: 0x40,
  Fua: 0x80,
} as const;

export const RadosLockFlag = { Renew: 1, MustRenew: 2 } as const;
export const RadosSnapshot = { Head: -2n, Directory: -1n } as const;
