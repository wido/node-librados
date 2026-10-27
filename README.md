# node-librados

[![npm version](https://img.shields.io/npm/v/node-librados.svg)](https://www.npmjs.com/package/node-librados)
[![npm downloads](https://img.shields.io/npm/dm/node-librados.svg)](https://www.npmjs.com/package/node-librados)
[![CI](https://github.com/SourceRegistry/node-librados/actions/workflows/ci.yml/badge.svg)](https://github.com/SourceRegistry/node-librados/actions/workflows/ci.yml)
[![node](https://img.shields.io/node/v/node-librados.svg)](https://www.npmjs.com/package/node-librados)
[![license](https://img.shields.io/npm/l/node-librados.svg)](LICENSE)

Native Node.js bindings for Ceph's RADOS object store and RBD block-image APIs. The package is binary-safe, asynchronous, explicit about native resource lifetime, and event-driven wherever Ceph supplies callbacks.

## Requirements

- Linux x86-64 or ARM64
- Node.js 22 or 24
- Ceph Squid 19 or Tentacle 20 client libraries (`librados2` and `librbd1`)

The npm package ships an N-API addon, not private copies of Ceph. On Debian or Ubuntu hosts, install the Ceph client libraries from the repository matching the cluster release.

## Quick start

```ts
import {Cluster, RbdFeature} from "node-librados";

await using cluster = await Cluster.connect({
  userName: "client.node-librados",
  configFile: "/etc/ceph/ceph.conf",
});

console.log(await cluster.stats());

await using io = cluster.ioContext("vms");
const rbd = io.rbd();
await rbd.create("vm-100", 64n * 1024n ** 3n, {
  features: RbdFeature.Layering | RbdFeature.ExclusiveLock | RbdFeature.ObjectMap,
});

await using image = await rbd.open("vm-100");
console.log(await image.stat());
```

All Ceph sizes, offsets, counters, IDs, and versions are represented as `bigint`. Object and image data is always a `Buffer`.

## RADOS

`Cluster` handles config files, CephX identity, connection state, cluster statistics, pool lifecycle, service registration, monitor pings, blocklisting, and raw monitor/manager/OSD/PG commands. `IoContext` exposes object I/O, xattrs, OMAP, namespaces, locators, snapshots, locks, listing, watch/notify, and RBD access.

```ts
await io.writeFull("metadata", Buffer.from("hello"));
await io.setOmap("metadata", {state: Buffer.from("ready")});

for await (const object of io.objects()) {
  console.log(object.namespace, object.name);
}
```

A compound write applies all of its steps to one object atomically, or none of them. Checks such as an exclusive create or an xattr comparison make it conditional:

```ts
await io.writeOp("payload")
  .create({exclusive: true})
  .writeFull(data)
  .setXattr("checksum", checksum)
  .operate();

await io.writeOp("payload")
  .compareXattr("state", "eq", Buffer.from("pending"))
  .setXattr("state", Buffer.from("done"))
  .operate();
```

Administrative commands are deliberately marked unsafe because their JSON schema and authorization are controlled by the connected Ceph release:

```ts
const health = await cluster.unsafeCommand(
  {kind: "mon"},
  {prefix: "health", format: "json"},
);
```

## Events

The module does not run background inventory or health polling. It translates native callbacks into bounded `EventEmitter` subscriptions.

```ts
const watch = io.watch("control-object", {autoAck: false});
watch.on("notification", async notification => {
  await notification.ack(Buffer.from("accepted"));
});
watch.on("overflow", dropped => console.warn("dropped", dropped));
watch.on("error", console.error);

const updates = image.watchUpdates();
updates.on("update", ({coalesced}) => console.log("image changed", coalesced));

// Explicit teardown is required in normal operation.
await updates.close();
await watch.close();
```

Queues default to 1,024 callbacks. RBD update signals are coalesced when JavaScript falls behind. Overflowed RADOS notifications are acknowledged empty so the notifying client cannot hang, and the next delivered event reports the loss.

Only one monitor-log callback can be active for a given native cluster handle:

```ts
const logs = cluster.monitorLogs({level: "warn"});
logs.on("log", entry => console.log(entry.channel, entry.message));
logs.on("overflow", dropped => console.warn({dropped}));
```

## Destructive operations

Typed destructive methods require a matching confirmation:

```ts
await cluster.deletePool("old-pool", {confirm: "old-pool"});
await rbd.remove("old-image", {confirm: "old-image"});
```

## Development

Open the included devcontainer, then run:

```sh
npm ci
npm run build
npm test
```

Integration tests use an explicitly configured disposable Ceph cluster:

```sh
CEPH_INTEGRATION=1 \
CEPH_CONF=/etc/ceph/ceph.conf \
CEPH_CLIENT=client.admin \
npm run test:integration
```

The integration suite creates and deletes a uniquely named pool. Never point it at a cluster where the configured identity should not have pool-management permissions.

## Deliberate boundaries

This package binds RADOS and RBD. CephFS, RGW/S3, Dashboard HTTP, cephadm orchestration, krbd mapping, and `rbd-nbd` process management belong in separate adapters. QEMU can consume RBD images directly.
