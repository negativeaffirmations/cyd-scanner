// One GATT write at a time. Android's stack runs a single GATT operation per connection and rejects a
// second one issued while the first is in flight -- Chrome reports it as "GATT operation failed for
// unknown reason". The periodic GPS feed and the file explorer's chunk stream both go through here so
// they take turns instead of colliding.
let tail = Promise.resolve();

export function gattWrite(ch, data){
  const run = tail.then(() => ch.writeValue(data));
  tail = run.catch(() => {});   // a failed write must not block the ones queued behind it
  return run;
}
