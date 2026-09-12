// Rae VS Code extension — syntax highlighting (declarative, see package.json)
// plus a document formatter backed by the compiler (#919): `rae format --stdin
// --stdout` is the ONE layout authority, so the editor never carries its own
// rules. Format-on-save is enabled for Rae files by the extension's
// `configurationDefaults`; the binary is found via `rae.binaryPath`, then
// `RAE_BIN`, then `rae` on PATH.
const vscode = require("vscode");
const { execFile } = require("child_process");

function raeBinary() {
  const configured = vscode.workspace.getConfiguration("rae").get("binaryPath");
  if (configured && configured.length > 0) return configured;
  if (process.env.RAE_BIN && process.env.RAE_BIN.length > 0) return process.env.RAE_BIN;
  return "rae";
}

function formatSource(source, fileName) {
  return new Promise((resolve, reject) => {
    const child = execFile(
      raeBinary(),
      ["format", "--stdin", "--stdout"],
      { maxBuffer: 16 * 1024 * 1024, cwd: vscode.workspace.rootPath || undefined },
      (error, stdout, stderr) => {
        if (error) {
          // A parse error (the file is mid-edit) or an over-cap refusal: leave
          // the document alone and surface the compiler's message.
          reject(new Error((stderr || error.message).trim().replace(/<stdin>/g, fileName)));
          return;
        }
        resolve(stdout);
      }
    );
    child.stdin.on("error", () => {});
    child.stdin.end(source);
  });
}

function activate(context) {
  const provider = {
    async provideDocumentFormattingEdits(document) {
      const source = document.getText();
      let formatted;
      try {
        formatted = await formatSource(source, document.fileName);
      } catch (err) {
        vscode.window.setStatusBarMessage(`rae format: ${err.message.split("\n")[0]}`, 5000);
        return [];
      }
      if (formatted === source) return [];
      const whole = new vscode.Range(document.positionAt(0), document.positionAt(source.length));
      return [vscode.TextEdit.replace(whole, formatted)];
    },
  };
  context.subscriptions.push(
    vscode.languages.registerDocumentFormattingEditProvider({ language: "rae", scheme: "file" }, provider),
    vscode.languages.registerDocumentFormattingEditProvider({ language: "rae", scheme: "untitled" }, provider)
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
