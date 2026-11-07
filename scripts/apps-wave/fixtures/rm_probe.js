const fs = require('fs'), os = require('os'), path = require('path');
function attempt(label, remove, watch) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'alchemy-rm-'));
  fs.writeFileSync(path.join(dir, 'a.txt'), 'x');
  fs.mkdirSync(path.join(dir, 'sub')); fs.writeFileSync(path.join(dir, 'sub', 'b.txt'), 'y');
  let watcher; if (watch) { watcher = fs.watch(dir, () => {}); watcher.close(); }
  let result = 'ok';
  try { remove(dir); } catch (e) { result = `${e.code} ${e.syscall || ''}`; }
  const left = fs.existsSync(dir) ? (fs.readdirSync(dir, { recursive: true }).join(',') || '(empty dir)') : '(gone)';
  console.log(`${label.padEnd(28)} ${result.padEnd(16)} left: ${left}`);
}
const manual = (dir) => { fs.unlinkSync(path.join(dir, 'sub', 'b.txt')); fs.rmdirSync(path.join(dir, 'sub')); fs.unlinkSync(path.join(dir, 'a.txt')); fs.rmdirSync(dir); };
attempt('rmSync recursive', (d) => fs.rmSync(d, { recursive: true, force: true }), false);
attempt('unlink + rmdir by hand', manual, false);
attempt('rmSync after fs.watch', (d) => fs.rmSync(d, { recursive: true, force: true }), true);
attempt('by hand after fs.watch', manual, true);
