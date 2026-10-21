// Alchemy probe extension (loaded with --extensionDevelopmentPath by
// scripts/vscode/run_vscode.ps1 -Probe). Through the public extension API only,
// no input: it opens a file of the workspace, runs a text search (ripgrep in
// VS Code's search service) and shows it in the Search view, and starts an
// integrated terminal (node-pty) whose process prints a line. Every step's
// outcome goes to <workspace>/.alchemy-probe.json, and the window title gets
// "probe done" when it is finished.
const vscode = require('vscode');
const fs = require('fs');
const path = require('path');

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function step(report, name, action) {
  const started = Date.now();
  try {
    report[name] = { ok: true, value: await action(), ms: Date.now() - started };
  } catch (error) {
    report[name] = { ok: false, error: String(error && error.stack || error), ms: Date.now() - started };
  }
}

async function activate() {
  const folder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
  if (!folder) return;
  const report = { started: new Date().toISOString() };
  const save = () => fs.writeFileSync(path.join(folder.uri.fsPath, '.alchemy-probe.json'), JSON.stringify(report, null, 2));
  const settings = vscode.workspace.getConfiguration('alchemyProbe');
  const file = settings.get('file') || 'hello.ts';
  const query = settings.get('query') || 'greet';

  await step(report, 'openFile', async () => {
    const document = await vscode.workspace.openTextDocument(vscode.Uri.joinPath(folder.uri, file));
    const editor = await vscode.window.showTextDocument(document, { preview: false });
    return { languageId: document.languageId, lines: document.lineCount, visible: editor.visibleRanges.length };
  });
  save();

  await step(report, 'findFiles', async () => {
    const files = await vscode.workspace.findFiles('**/*', '**/node_modules/**', 100);
    return files.map((uri) => path.basename(uri.fsPath)).sort();
  });
  save();

  await step(report, 'textSearch', async () => {
    const matches = [];
    if (vscode.workspace.findTextInFiles) {
      await vscode.workspace.findTextInFiles({ pattern: query }, {}, (result) => {
        matches.push({ file: path.basename(result.uri.fsPath), preview: result.preview ? result.preview.text.trim() : '' });
      });
    }
    return matches;
  });
  save();

  await step(report, 'searchView', async () => {
    await vscode.commands.executeCommand('workbench.action.findInFiles', { query, triggerSearch: true });
    return 'shown';
  });
  save();

  if (settings.get('terminal') !== false) {
    // What the terminals write (the terminalDataWriteEvent proposal), by terminal name.
    const written = {};
    const writes = vscode.window.onDidWriteTerminalData
      ? vscode.window.onDidWriteTerminalData((event) => { written[event.terminal.name] = (written[event.terminal.name] || '') + event.data; })
      : undefined;
    // Electron's Node as the terminal's process (no guest shell is needed).
    const node = (name, script) => vscode.window.createTerminal({
      name, shellPath: process.execPath, shellArgs: ['-e', script], env: { ELECTRON_RUN_AS_NODE: '1' },
    });
    await step(report, 'terminal', async () => {
      const closed = new Promise((resolve) => {
        const subscription = vscode.window.onDidCloseTerminal((terminal) => {
          if (terminal.name === 'alchemy probe') { subscription.dispose(); resolve(terminal.exitStatus); }
        });
      });
      const terminal = node('alchemy probe', 'console.log("hello from the integrated terminal", process.platform, process.arch, ' +
        'process.stdout.isTTY ? "tty " + process.stdout.columns + "x" + process.stdout.rows : "not a tty"); setTimeout(() => {}, 4000)');
      terminal.show(true);
      const pid = await Promise.race([terminal.processId, delay(30000).then(() => 'no process id after 30 s')]);
      const status = await Promise.race([closed, delay(60000).then(() => 'still open after 60 s')]);
      return { pid, exitStatus: status, output: written['alchemy probe'] };
    });
    save();
    // A terminal that stays open, for the window capture.
    await step(report, 'terminalShown', async () => {
      const terminal = node('alchemy shown', 'console.log("Node", process.version, "in a pseudo-terminal:", process.stdout.isTTY, ' +
        'process.stdout.columns + "x" + process.stdout.rows); setInterval(() => {}, 60000)');
      terminal.show(true);
      const pid = await Promise.race([terminal.processId, delay(30000).then(() => 'no process id after 30 s')]);
      for (let waited = 0; waited < 20000 && !(written['alchemy shown'] || '').includes('\n'); waited += 250) await delay(250);
      return { pid, output: written['alchemy shown'] };
    });
    if (writes) writes.dispose();
    save();
  }

  report.finished = new Date().toISOString();
  save();
  // The harness waits for this title.
  await vscode.workspace.getConfiguration('window').update('title', 'probe done - ${activeEditorShort}', vscode.ConfigurationTarget.Workspace);
}

function deactivate() {}

module.exports = { activate, deactivate };
