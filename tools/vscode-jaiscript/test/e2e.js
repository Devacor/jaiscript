// End-to-end check inside a real VS Code: the extension activates on a .jai file, launches
// jaiscript_lsp, and diagnostics, definition, hover, completion and the debugger type all answer.
//
//   code --user-data-dir <tmp> --extensions-dir <tmp> --disable-extensions \
//        --extensionDevelopmentPath=<this extension> --extensionTestsPath=<this folder>/e2e.js \
//        <examples/gatorquest>
//
// Results go to JAISCRIPT_E2E_OUT (default: e2e_result.json beside this file); the process exit
// code is the verdict.

const fs = require('fs');
const path = require('path');
const vscode = require('vscode');

function sleep(ms) { return new Promise((resolve) => setTimeout(resolve, ms)); }

// A wait for what must happen fails only past this guard against a hang: it never bounds speed
const HANG_GUARD_MS = 10 * 60 * 1000;

async function waitFor(what, test) {
	const deadline = performance.now() + HANG_GUARD_MS;
	for (;;) {
		const value = await test();
		if (value) { return value; }
		if (performance.now() > deadline) { throw new Error('hung waiting for ' + what); }
		await sleep(200);
	}
}

async function run() {
	const results = { checks: [] };
	const out = process.env.JAISCRIPT_E2E_OUT || path.join(__dirname, 'e2e_result.json');
	const check = (name, ok, detail) => { results.checks.push({ name, ok: !!ok, detail: detail === undefined ? '' : String(detail).slice(0, 400) }); };
	try {
		const root = vscode.workspace.workspaceFolders[0].uri.fsPath;
		const file = vscode.Uri.file(path.join(root, 'scripts', 'gatorquest.jai'));
		const doc = await vscode.workspace.openTextDocument(file);
		await vscode.window.showTextDocument(doc);
		check('language id', doc.languageId === 'jaiscript', doc.languageId);
		const ext = vscode.extensions.getExtension('snapjaw.jaiscript');
		if (ext) { await waitFor('activation', async () => ext.isActive).catch(() => {}); }
		check('extension active', ext && ext.isActive, ext ? ext.isActive : 'missing');

		const text = doc.getText();
		const at = (needle, delta) => doc.positionAt(text.indexOf(needle) + (delta || 0));

		const definition = await waitFor('definition', async () => {
			const r = await vscode.commands.executeCommand('vscode.executeDefinitionProvider', file, at('CHARS[slot]'));
			return r && r.length ? r : undefined;
		});
		const target = definition[0].uri ? definition[0] : { uri: definition[0].targetUri, range: definition[0].targetRange };
		check('definition CHARS -> gq_state.jai:103', path.basename(target.uri.fsPath) === 'gq_state.jai' && target.range.start.line === 102,
			path.basename(target.uri.fsPath) + ':' + (target.range.start.line + 1));

		const hovers = await vscode.commands.executeCommand('vscode.executeHoverProvider', file, at('mob_live(m)'));
		const hoverText = (hovers || []).map((h) => h.contents.map((c) => c.value || c).join('\n')).join('\n');
		check('hover mob_live signature', hoverText.includes('bool mob_live(int m)'), hoverText);

		const refs = await vscode.commands.executeCommand('vscode.executeReferenceProvider', file, at('mob_live(m)'));
		check('references mob_live across files', refs && refs.length >= 30 && new Set(refs.map((r) => r.uri.fsPath)).size >= 5, refs ? refs.length : 0);

		const edit = new vscode.WorkspaceEdit();
		const insertAt = at('string skills');
		edit.insert(file, insertAt, 'c.\n\t');
		await vscode.workspace.applyEdit(edit);
		const completion = await vscode.commands.executeCommand('vscode.executeCompletionItemProvider', file, new vscode.Position(insertAt.line, insertAt.character + 2), '.');
		const labels = new Set((completion ? completion.items : []).map((i) => typeof i.label === 'string' ? i.label : i.label.label));
		check('completion c. lists Character fields', ['pack', 'gold', 'skill', 'name'].every((l) => labels.has(l)), [...labels].slice(0, 20).join(','));
		const remove = new vscode.WorkspaceEdit();
		remove.delete(file, new vscode.Range(insertAt, new vscode.Position(insertAt.line + 1, 1)));
		await vscode.workspace.applyEdit(remove);

		const probe = 'int typo_probe = "text";\n\t';
		const broken = new vscode.WorkspaceEdit();
		broken.insert(file, insertAt, probe);
		await vscode.workspace.applyEdit(broken);
		const diagnostics = await waitFor('the typed error', async () => {
			const list = vscode.languages.getDiagnostics(file);
			return list.some((d) => d.message.includes('typo_probe')) ? list : undefined;
		});
		check('typing a type error shows its diagnostic', diagnostics.some((d) => d.message.includes("cannot initialize 'typo_probe'")), diagnostics.map((d) => d.message).join(' | '));
		const fix = new vscode.WorkspaceEdit();
		fix.delete(file, new vscode.Range(insertAt, new vscode.Position(insertAt.line + 1, 1)));
		await vscode.workspace.applyEdit(fix);
		await waitFor('the diagnostic to clear', async () => !vscode.languages.getDiagnostics(file).some((d) => d.message.includes('typo_probe')));
		check('fixing it clears the diagnostic', vscode.languages.getDiagnostics(file).length === 0, vscode.languages.getDiagnostics(file).map((d) => d.message).join(' | '));

		const importedFile = vscode.Uri.file(path.join(root, 'scripts', 'gq_util.jai'));
		const leaf = await vscode.workspace.openTextDocument(importedFile);
		const leafEdit = new vscode.WorkspaceEdit();
		leafEdit.insert(importedFile, new vscode.Position(leaf.lineCount - 1, 0), 'int leaf_probe = "text";\n');
		await vscode.workspace.applyEdit(leafEdit);
		const leafDiagnostics = await waitFor('the imported file\'s error', async () => {
			const list = vscode.languages.getDiagnostics(importedFile);
			return list.some((d) => d.message.includes('leaf_probe')) ? list : undefined;
		});
		const leafLine = leafDiagnostics.find((d) => d.message.includes('leaf_probe')).range.start.line;
		check('an error in an imported file is reported in that file at its line', leafLine === leaf.lineCount - 2, 'line ' + (leafLine + 1) + ' of ' + leaf.lineCount);

		const debuggers = ext ? ext.packageJSON.contributes.debuggers.map((d) => d.type) : [];
		check('debugger type still contributed', debuggers.includes('jaiscript'), debuggers.join(','));
		await vscode.commands.executeCommand('workbench.action.revertAndCloseActiveEditor');
	} catch (err) {
		check('no exception', false, err && err.stack ? err.stack : err);
	}
	fs.writeFileSync(out, JSON.stringify(results, null, 1));
	const failed = results.checks.filter((c) => !c.ok);
	if (failed.length) { throw new Error(failed.length + ' check(s) failed: ' + failed.map((c) => c.name).join(', ')); }
}

module.exports = { run };
