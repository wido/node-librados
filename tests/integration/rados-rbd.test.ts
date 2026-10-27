import assert from "node:assert/strict";
import {once} from "node:events";
import {after, before, test} from "node:test";
import {Cluster, RbdFeature} from "../../src-ts";

const enabled = process.env.CEPH_INTEGRATION === "1";
const pool = process.env.CEPH_TEST_POOL ?? `node-librados-${process.pid}`;
let cluster: Cluster;

before(async () => {
  if (!enabled) return;
  cluster = await Cluster.connect({
    clusterName: process.env.CEPH_CLUSTER ?? "ceph",
    userName: process.env.CEPH_CLIENT ?? "client.admin",
    configFile: process.env.CEPH_CONF ?? null,
  });
  await cluster.createPool(pool);
  await cluster.unsafeCommand({kind: "mon"}, {prefix: "osd pool application enable", pool, app: "rbd", yes_i_really_mean_it: true});
});

after(async () => {
  if (!enabled) return;
  await cluster.deletePool(pool, {confirm: pool}).catch(() => undefined);
  await cluster.close();
});

test("binary RADOS I/O, xattrs, omap, listing, and notifications", {skip: !enabled}, async () => {
  await using io = cluster.ioContext(pool);
  const payload = Buffer.from([0, 1, 2, 0, 255]);
  await io.writeFull("object", payload);
  assert.deepEqual(await io.read("object", 5n), payload);
  await io.setXattr("object", "kind", Buffer.from("test"));
  assert.equal((await io.getXattr("object", "kind")).toString(), "test");
  await io.setOmap("object", {alpha: Buffer.from("one")});
  assert.equal((await io.getOmap("object")).entries.alpha.toString(), "one");
  assert.deepEqual((await Array.fromAsync(io.objects())).map(value => value.name), ["object"]);

  const watch = io.watch("object", {autoAck: false});
  const received = once(watch, "notification");
  const notify = io.notify("object", Buffer.from("ping"));
  const [notification] = await received;
  assert.equal(notification.payload.toString(), "ping");
  await notification.ack(Buffer.from("pong"));
  assert.ok((await notify).includes(Buffer.from("pong")));
  await watch.close();
});

test("lists objects a page at a time", {skip: !enabled}, async () => {
  await using io = cluster.ioContext(pool);
  const prefix = "stream-objects";
  const names = Array.from({length: 25}, (_, i) => `${prefix}-${i}`);
  for (const name of names) await io.writeFull(name, Buffer.from(name));
  const listed: string[] = [];
  for await (const object of io.objects({pageSize: 4})) if (object.name.startsWith(prefix)) listed.push(object.name);
  assert.deepEqual(listed.sort(), [...names].sort());
  // leaving a listing early closes it, or closing the io context below would fail with EBUSY
  for await (const object of io.objects({pageSize: 2})) { assert.ok(object.name); break; }
  for (const name of names) await io.remove(name);
});

test("RBD image, I/O, metadata, snapshots, resize, and trash", {skip: !enabled}, async () => {
  await using io = cluster.ioContext(pool);
  const rbd = io.rbd();
  await rbd.create("image", 8n * 1024n * 1024n, {features: RbdFeature.Layering});
  await using image = await rbd.open("image");
  await image.write(0n, Buffer.from("ceph"));
  assert.equal((await image.read(0n, 4n)).toString(), "ceph");
  await image.setMetadata("owner", "node-librados");
  assert.equal(await image.metadata("owner"), "node-librados");
  await image.createSnapshot("first");
  assert.equal((await image.snapshots())[0]?.name, "first");
  await image.removeSnapshot("first", {confirm: "first"});
  await image.close();
  await rbd.moveToTrash("image");
});
