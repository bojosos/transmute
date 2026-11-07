// Node check: a child process, a worker thread, a pipe through a child, and a signal to a child.
const { execFileSync, spawn } = require('child_process');
const { Worker } = require('worker_threads');
console.log('child:', execFileSync(process.execPath, ['-p', '1+1']).toString().trim());
const worker = new Worker("require('worker_threads').parentPort.postMessage(6 * 7)", { eval: true });
worker.on('message', (m) => {
  console.log('worker:', m);
  const cat = spawn(process.execPath, ['-e', "process.stdin.pipe(process.stdout)"]);
  let piped = '';
  cat.stdout.on('data', (d) => { piped += d; });
  cat.on('close', (code) => {
    console.log('pipe:', piped.trim(), 'code', code);
    const sleeper = spawn(process.execPath, ['-e', 'setTimeout(() => {}, 60000)']);
    setTimeout(() => sleeper.kill('SIGTERM'), 500);
    sleeper.on('exit', (code, signal) => console.log('signal:', signal, 'code', code));
  });
  cat.stdin.end('through a pipe\n');
});
