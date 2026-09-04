import type { NativeClipboard } from "@earendil-works/pi-tui";
import { execFileSync, execSync, spawn } from "child_process";
import { platform } from "os";
import { afterEach, beforeEach, describe, expect, test, vi } from "vitest";
import { copyToClipboard, readClipboardText } from "../src/utils/clipboard.ts";

const mocks = vi.hoisted(() => ({
	clipboard: {
		getText: vi.fn<() => string | null>(),
		getImage: vi.fn<() => Uint8Array | null>(),
		setText: vi.fn<(text: string) => void>(),
	},
	getNativeClipboard: vi.fn<(backend?: "wayland" | "x11") => NativeClipboard | undefined>(),
	execFileSync: vi.fn(),
	execSync: vi.fn(),
	spawn: vi.fn(),
	platform: vi.fn<() => NodeJS.Platform>(),
	isWaylandSession: vi.fn<() => boolean>(),
}));

vi.mock("@earendil-works/pi-tui", () => ({ getNativeClipboard: mocks.getNativeClipboard }));
vi.mock("child_process", () => ({ execFileSync: mocks.execFileSync, execSync: mocks.execSync, spawn: mocks.spawn }));
vi.mock("os", () => ({ platform: mocks.platform }));
vi.mock("../src/utils/clipboard-image.ts", () => ({ isWaylandSession: mocks.isWaylandSession }));

const mockedExecFileSync = vi.mocked(execFileSync);
const mockedExecSync = vi.mocked(execSync);
const mockedSpawn = vi.mocked(spawn);
const mockedPlatform = vi.mocked(platform);
let originalWrite: typeof process.stdout.write;
let osc52Writes: string[];

beforeEach(() => {
	vi.resetAllMocks();
	for (const name of [
		"SSH_CONNECTION",
		"SSH_CLIENT",
		"MOSH_CONNECTION",
		"WAYLAND_DISPLAY",
		"DISPLAY",
		"TERMUX_VERSION",
	]) {
		vi.stubEnv(name, "");
	}
	osc52Writes = [];
	mockedPlatform.mockReturnValue("darwin");
	mocks.getNativeClipboard.mockReturnValue(mocks.clipboard);
	mocks.clipboard.getText.mockReturnValue(null);
	originalWrite = process.stdout.write.bind(process.stdout);
	process.stdout.write = ((...args: Parameters<typeof process.stdout.write>) => {
		const [chunk] = args;
		if (typeof chunk === "string" && chunk.startsWith("\x1b]52;c;")) {
			osc52Writes.push(chunk);
			return true;
		}
		return originalWrite(...args);
	}) as typeof process.stdout.write;
});

afterEach(() => {
	process.stdout.write = originalWrite;
	vi.unstubAllEnvs();
});

describe("readClipboardText", () => {
	test("returns native clipboard text", async () => {
		mocks.clipboard.getText.mockReturnValue("clipboard text");
		await expect(readClipboardText()).resolves.toBe("clipboard text");
	});

	test("reads the Wayland clipboard without consulting X11 tools", async () => {
		// Regression test for #7248.
		mockedPlatform.mockReturnValue("linux");
		vi.stubEnv("WAYLAND_DISPLAY", "wayland-0");
		vi.stubEnv("DISPLAY", ":0");
		mockedExecFileSync.mockReturnValue("Wayland text");

		await expect(readClipboardText()).resolves.toBe("Wayland text");
		expect(mockedExecFileSync).toHaveBeenCalledOnce();
		expect(mockedExecFileSync).toHaveBeenCalledWith("wl-paste", ["--no-newline", "--type", "text"], {
			encoding: "utf8",
			maxBuffer: 50 * 1024 * 1024,
			timeout: 5000,
		});
		expect(mocks.getNativeClipboard).not.toHaveBeenCalled();
	});

	for (const text of ["native Wayland text", "", null]) {
		test(`native Wayland result ${JSON.stringify(text)} takes precedence over X11`, async () => {
			mockedPlatform.mockReturnValue("linux");
			vi.stubEnv("WAYLAND_DISPLAY", "wayland-0");
			vi.stubEnv("DISPLAY", ":0");
			mockedExecFileSync.mockImplementation((command) => {
				if (command === "wl-paste") throw new Error("wl-paste unavailable");
				return "stale X11 text";
			});
			mocks.clipboard.getText.mockReturnValue(text);

			await expect(readClipboardText()).resolves.toBe(text || null);
			expect(mocks.getNativeClipboard).toHaveBeenCalledExactlyOnceWith("wayland");
			expect(mockedExecFileSync).toHaveBeenCalledOnce();
		});
	}

	test("an empty wl-paste result stops fallback", async () => {
		mockedPlatform.mockReturnValue("linux");
		vi.stubEnv("WAYLAND_DISPLAY", "wayland-0");
		vi.stubEnv("DISPLAY", ":0");
		mockedExecFileSync.mockReturnValue("");
		await expect(readClipboardText()).resolves.toBeNull();
		expect(mockedExecFileSync).toHaveBeenCalledOnce();
		expect(mocks.getNativeClipboard).not.toHaveBeenCalled();
	});

	test("falls back to X11 only when Wayland tools and the native backend are unavailable", async () => {
		mockedPlatform.mockReturnValue("linux");
		vi.stubEnv("WAYLAND_DISPLAY", "wayland-0");
		vi.stubEnv("DISPLAY", ":0");
		mocks.getNativeClipboard.mockReturnValue(undefined);
		mockedExecFileSync.mockImplementation((command) => {
			if (command === "wl-paste") throw new Error("wl-paste unavailable");
			return "X11 text";
		});
		await expect(readClipboardText()).resolves.toBe("X11 text");
		expect(mocks.getNativeClipboard).toHaveBeenCalledExactlyOnceWith("wayland");
	});

	test("falls back from xclip to xsel on X11", async () => {
		mockedPlatform.mockReturnValue("linux");
		vi.stubEnv("DISPLAY", ":0");
		mockedExecFileSync.mockImplementation((command) => {
			if (command === "xclip") throw new Error("xclip unavailable");
			return "xsel text";
		});
		await expect(readClipboardText()).resolves.toBe("xsel text");
		expect(mockedExecFileSync).toHaveBeenLastCalledWith("xsel", ["--clipboard", "--output"], {
			encoding: "utf8",
			maxBuffer: 50 * 1024 * 1024,
			timeout: 5000,
		});
		expect(mocks.getNativeClipboard).not.toHaveBeenCalled();
	});

	test("falls back to native X11 when clipboard tools are unavailable", async () => {
		mockedPlatform.mockReturnValue("linux");
		vi.stubEnv("DISPLAY", ":0");
		mockedExecFileSync.mockImplementation(() => {
			throw new Error("tool unavailable");
		});
		mocks.clipboard.getText.mockReturnValue("native X11 text");
		await expect(readClipboardText()).resolves.toBe("native X11 text");
		expect(mocks.getNativeClipboard).toHaveBeenCalledExactlyOnceWith("x11");
	});

	test("reads clipboard text through Termux:API", async () => {
		mockedPlatform.mockReturnValue("linux");
		vi.stubEnv("TERMUX_VERSION", "0.119");
		mockedExecFileSync.mockReturnValue("Termux text");
		await expect(readClipboardText()).resolves.toBe("Termux text");
		expect(mockedExecFileSync).toHaveBeenCalledWith("termux-clipboard-get", [], {
			encoding: "utf8",
			maxBuffer: 50 * 1024 * 1024,
			timeout: 5000,
		});
		expect(mocks.getNativeClipboard).not.toHaveBeenCalled();
	});

	test("returns null for empty or unavailable clipboard text", async () => {
		await expect(readClipboardText()).resolves.toBeNull();
		mocks.clipboard.getText.mockImplementation(() => {
			throw new Error("clipboard unavailable");
		});
		await expect(readClipboardText()).resolves.toBeNull();
	});
});

describe("copyToClipboard", () => {
	test("local native success skips OSC 52 and shell fallbacks", async () => {
		await copyToClipboard("hello");
		expect(mocks.clipboard.setText).toHaveBeenCalledWith("hello");
		expect(osc52Writes).toHaveLength(0);
		expect(mockedExecSync).not.toHaveBeenCalled();
		expect(mockedSpawn).not.toHaveBeenCalled();
	});

	test("Linux skips the native writer", async () => {
		mockedPlatform.mockReturnValue("linux");
		vi.stubEnv("DISPLAY", ":0");
		await copyToClipboard("hello");
		expect(mocks.getNativeClipboard).not.toHaveBeenCalled();
		expect(mockedExecSync).toHaveBeenCalledWith("xclip -selection clipboard", {
			input: "hello",
			stdio: ["pipe", "ignore", "ignore"],
			timeout: 5000,
		});
	});

	test("remote native success emits OSC 52 after the native write", async () => {
		vi.stubEnv("SSH_CONNECTION", "client server");
		mocks.clipboard.setText.mockImplementation(() => {
			expect(osc52Writes).toHaveLength(0);
		});
		await copyToClipboard("hello");
		expect(mocks.clipboard.setText).toHaveBeenCalledWith("hello");
		expect(osc52Writes).toHaveLength(1);
		expect(mockedExecSync).not.toHaveBeenCalled();
	});

	test("local shell fallback success skips OSC 52", async () => {
		mocks.clipboard.setText.mockImplementation(() => {
			throw new Error("native failed");
		});
		await copyToClipboard("hello");
		expect(mockedExecSync).toHaveBeenCalledWith("pbcopy", {
			input: "hello",
			stdio: ["pipe", "ignore", "ignore"],
			timeout: 5000,
		});
		expect(osc52Writes).toHaveLength(0);
	});

	test("a read-only native clipboard uses the shell writer", async () => {
		mocks.getNativeClipboard.mockReturnValue({
			getText: mocks.clipboard.getText,
			getImage: mocks.clipboard.getImage,
		});
		await copyToClipboard("hello");
		expect(mockedExecSync).toHaveBeenCalledOnce();
	});

	test("uses OSC 52 fallback when native and shell tools fail", async () => {
		mocks.clipboard.setText.mockImplementation(() => {
			throw new Error("native failed");
		});
		mockedExecSync.mockImplementation(() => {
			throw new Error("pbcopy failed");
		});
		await copyToClipboard("hello");
		expect(osc52Writes).toHaveLength(1);
	});

	test("does not emit oversized OSC 52 payloads", async () => {
		mocks.clipboard.setText.mockImplementation(() => {
			throw new Error("native failed");
		});
		mockedExecSync.mockImplementation(() => {
			throw new Error("pbcopy failed");
		});
		await expect(copyToClipboard("x".repeat(80_000))).rejects.toThrow("Failed to copy to clipboard");
		expect(osc52Writes).toHaveLength(0);
	});
});
