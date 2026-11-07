// Node check: modules, timers, promises, fs, child_process and worker threads.
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const file = path.join(os.tmpdir(), `alchemy-node-${process.pid}.txt`);
fs.writeFileSync(file, 'hello file\n');
const text = fs.readFileSync(file, 'utf8').trim();
fs.unlinkSync(file);
const digest = crypto.createHash('sha256').update('abc').digest('hex').slice(0, 16);
setTimeout(async () => {
  const value = await Promise.resolve(42);
  console.log(`hello from node ${process.version} ${process.platform}/${process.arch}: ${text}, sha=${digest}, value=${value}`);
}, 10);
