import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import { LanguageClient, LanguageClientOptions, ServerOptions, TransportKind } from 'vscode-languageclient/node';

// Two halves. The debugger uses the attach model with no bundled adapter process: the DAP server
// lives inside the C++ host (jai::debug::listen opens a TCP port), so VS Code's DAP client is
// pointed straight at that socket. The language server (jaiscript_lsp) is a separate executable
// speaking LSP over stdio; this extension launches it for every .jai file.

let client: LanguageClient | undefined;

function serverExecutable(context: vscode.ExtensionContext): string | undefined {
	const exe = process.platform === 'win32' ? 'jaiscript_lsp.exe' : 'jaiscript_lsp';
	const configured = vscode.workspace.getConfiguration('jaiscript').get<string>('languageServer.path', '').trim();
	if (configured) {
		const folder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? '';
		return configured.replace(/\$\{workspaceFolder\}/g, folder);
	}
	// A packaged extension carries the server beside it; run from a source checkout, the extension
	// sits in tools/vscode-jaiscript and the build trees are ../../build (CMake presets) and
	// ../../out/build/<config> (Visual Studio's Open Folder).
	const root = path.resolve(context.extensionPath, '..', '..');
	const candidates = [path.join(context.extensionPath, 'server', exe)];
	for (const tree of ['build', 'build-debug']) {
		candidates.push(path.join(root, tree, 'bin', exe));
	}
	for (const config of ['x64-Release', 'x64-Debug']) {
		candidates.push(path.join(root, 'out', 'build', config, 'bin', exe));
	}
	for (const candidate of candidates) {
		if (fs.existsSync(candidate)) {
			return candidate;
		}
	}
	for (const dir of (process.env.PATH ?? '').split(path.delimiter)) {
		const candidate = path.join(dir, exe);
		if (dir && fs.existsSync(candidate)) {
			return candidate;
		}
	}
	return undefined;
}

async function startLanguageServer(context: vscode.ExtensionContext): Promise<void> {
	const config = vscode.workspace.getConfiguration('jaiscript');
	if (!config.get<boolean>('languageServer.enabled', true)) {
		return;
	}
	const command = serverExecutable(context);
	if (!command) {
		vscode.window.showWarningMessage('JaiScript: jaiscript_lsp was not found. Build the jaiscript_lsp CMake target or set jaiscript.languageServer.path.');
		return;
	}
	const args: string[] = [];
	if (config.get<boolean>('languageServer.log', false)) {
		args.push('--log');
	}
	const serverOptions: ServerOptions = { command, args, transport: TransportKind.stdio };
	const clientOptions: LanguageClientOptions = {
		documentSelector: [{ scheme: 'file', language: 'jaiscript' }],
		synchronize: { fileEvents: vscode.workspace.createFileSystemWatcher('**/*.jai') },
		initializationOptions: {
			host: config.get<string>('languageServer.host', 'runner'),
			includePaths: config.get<string[]>('languageServer.includePaths', []),
		},
		outputChannelName: 'JaiScript Language Server',
	};
	client = new LanguageClient('jaiscript', 'JaiScript Language Server', serverOptions, clientOptions);
	await client.start();
}

async function stopLanguageServer(): Promise<void> {
	if (client) {
		const running = client;
		client = undefined;
		await running.stop();
	}
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
	const factory: vscode.DebugAdapterDescriptorFactory = {
		createDebugAdapterDescriptor(session: vscode.DebugSession): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
			const cfg = session.configuration;
			const port: number = cfg.port ?? 52477;
			const host: string = cfg.host ?? '127.0.0.1';
			return new vscode.DebugAdapterServer(port, host);
		}
	};
	context.subscriptions.push(vscode.debug.registerDebugAdapterDescriptorFactory('jaiscript', factory));

	context.subscriptions.push(vscode.commands.registerCommand('jaiscript.restartLanguageServer', async () => {
		await stopLanguageServer();
		await startLanguageServer(context);
	}));
	context.subscriptions.push(vscode.workspace.onDidChangeConfiguration(async (e) => {
		if (e.affectsConfiguration('jaiscript.languageServer')) {
			await stopLanguageServer();
			await startLanguageServer(context);
		}
	}));
	await startLanguageServer(context);
}

export async function deactivate(): Promise<void> {
	await stopLanguageServer();
}
