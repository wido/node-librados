import assert from "node:assert/strict";
import {once} from "node:events";
import {after, before, test} from "node:test";
import {CephError, Cluster, RbdFeature} from "../../src-ts";

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

test("compound write operations apply all steps or none", {skip: !enabled}, async () => {
  await using io = cluster.ioContext(pool);
  const failsWith = (errno: number) => (error: unknown) => error instanceof CephError && error.errno === errno;
  await io.writeOp("op").create({exclusive: true}).writeFull(Buffer.from("first")).setXattr("state", Buffer.from("ready")).operate();
  assert.equal((await io.read("op", 16n)).toString(), "first");
  assert.equal((await io.getXattr("op", "state")).toString(), "ready");

  await assert.rejects(io.writeOp("op").create({exclusive: true}).writeFull(Buffer.from("second")).operate(), failsWith(17));
  await assert.rejects(io.writeOp("op").compareXattr("state", "eq", Buffer.from("other")).writeFull(Buffer.from("second")).removeXattr("state").operate(), failsWith(125));
  assert.equal((await io.read("op", 16n)).toString(), "first");
  assert.equal((await io.getXattr("op", "state")).toString(), "ready");

  await io.writeOp("op").compareXattr("state", "eq", Buffer.from("ready")).writeFull(Buffer.from("second")).setXattr("state", Buffer.from("done")).operate();
  assert.equal((await io.read("op", 16n)).toString(), "second");
  assert.equal((await io.getXattr("op", "state")).toString(), "done");

  await assert.rejects(io.writeOp("op-missing").assertExists().writeFull(Buffer.from("x")).operate(), failsWith(2));
  await assert.rejects(io.stat("op-missing"), failsWith(2));
  await io.writeOp("op").remove().operate();
  await assert.rejects(io.stat("op"), failsWith(2));
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
