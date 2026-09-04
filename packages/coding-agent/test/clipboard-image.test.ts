import type { NativeClipboard } from "@earendil-works/pi-tui";
import type { SpawnSyncReturns } from "child_process";
import { writeFileSync } from "fs";
import { beforeEach, describe, expect, test, vi } from "vitest";
import { readClipboardImage } from "../src/utils/clipboard-image.ts";

const mocks = vi.hoisted(() => ({
	spawnSync: vi.fn<(command: string, args: string[], options: unknown) => SpawnSyncReturns<Buffer>>(),
	getImage: vi.fn<() => Uint8Array | null>(),
	getNativeClipboard: vi.fn<(backend?: "wayland" | "x11") => NativeClipboard | undefined>(),
}));

vi.mock("child_process", () => ({ spawnSync: mocks.spawnSync }));
vi.mock("@earendil-works/pi-tui", () => ({ getNativeClipboard: mocks.getNativeClipboard }));

function spawnResult(stdout: Buffer, status = 0): SpawnSyncReturns<Buffer> {
	return {
		pid: 123,
		output: [Buffer.alloc(0), stdout, Buffer.alloc(0)],
		stdout,
		stderr: Buffer.alloc(0),
		status,
		signal: null,
	};
}

const png = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 0x49, 0x48, 0x44, 0x52]);

describe("readClipboardImage", () => {
	beforeEach(() => {
		vi.resetAllMocks();
		mocks.spawnSync.mockReturnValue(spawnResult(Buffer.alloc(0), 1));
		mocks.getImage.mockReturnValue(png);
		mocks.getNativeClipboard.mockReturnValue({ getText: () => null, getImage: mocks.getImage });
	});

	test("Wayland: uses wl-paste without the native clipboard", async () => {
		mocks.spawnSync.mockImplementation((command, args) => {
			expect(command).toBe("wl-paste");
			return spawnResult(args[0] === "--list-types" ? Buffer.from("text/plain\nimage/png\n") : Buffer.from(png));
		});
		const result = await readClipboardImage({ platform: "linux", env: { WAYLAND_DISPLAY: "1" } });
		expect(result).toEqual({ bytes: png, mimeType: "image/png" });
		expect(mocks.getNativeClipboard).not.toHaveBeenCalled();
	});

	test("Wayland: tries the native reader before stale X11 images", async () => {
		const result = await readClipboardImage({ platform: "linux", env: { WAYLAND_DISPLAY: "1", DISPLAY: ":0" } });
		expect(result).toEqual({ bytes: png, mimeType: "image/png" });
		expect(mocks.getNativeClipboard).toHaveBeenCalledExactlyOnceWith("wayland");
		expect(mocks.getImage).toHaveBeenCalledOnce();
		expect(mocks.spawnSync).toHaveBeenCalledOnce();
	});

	for (const native of [false, true]) {
		test(`Wayland: an empty ${native ? "native" : "wl-paste"} clipboard does not fall through to X11`, async () => {
			if (native) mocks.getImage.mockReturnValue(null);
			else mocks.spawnSync.mockReturnValue(spawnResult(Buffer.from("text/plain\n")));
			expect(
				await readClipboardImage({ platform: "linux", env: { WAYLAND_DISPLAY: "1", DISPLAY: ":0" } }),
			).toBeNull();
			expect(mocks.spawnSync).toHaveBeenCalledOnce();
			if (native) expect(mocks.getNativeClipboard).toHaveBeenCalledExactlyOnceWith("wayland");
			else expect(mocks.getNativeClipboard).not.toHaveBeenCalled();
		});
	}

	test("Wayland: falls back to X11 when both Wayland readers are unavailable", async () => {
		mocks.getNativeClipboard.mockReturnValue(undefined);
		mocks.spawnSync.mockImplementation((command, args) => {
			if (command === "wl-paste") return spawnResult(Buffer.alloc(0), 1);
			return spawnResult(args.includes("TARGETS") ? Buffer.from("image/png\n") : Buffer.from(png));
		});
		expect(await readClipboardImage({ platform: "linux", env: { WAYLAND_DISPLAY: "1" } })).toEqual({
			bytes: png,
			mimeType: "image/png",
		});
		expect(mocks.getNativeClipboard).toHaveBeenCalledExactlyOnceWith("wayland");
	});

	test("WSL: passes the PowerShell path directly instead of through a custom env var", async () => {
		mocks.getImage.mockReturnValue(null);
		let tmpFile: string | undefined;
		mocks.spawnSync.mockImplementation((command, args, options) => {
			if (command === "wl-paste" || command === "xclip") return spawnResult(Buffer.alloc(0));
			if (command === "wslpath") {
				tmpFile = args[1];
				return spawnResult(Buffer.from("C:\\Users\\O'Hare\\clip.png\n"));
			}
			if (command === "powershell.exe") {
				const spawnOptions = options as { env?: NodeJS.ProcessEnv };
				expect(spawnOptions.env?.PI_WSL_CLIPBOARD_IMAGE_PATH).toBeUndefined();
				expect(args[2]).toContain("$path = 'C:\\Users\\O''Hare\\clip.png'");
				if (!tmpFile) throw new Error("wslpath should be called before powershell.exe");
				writeFileSync(tmpFile, png);
				return spawnResult(Buffer.from("ok\n"));
			}
			throw new Error(`Unexpected command: ${command}`);
		});
		expect(await readClipboardImage({ platform: "linux", env: { WSL_DISTRO_NAME: "Ubuntu" } })).toEqual({
			bytes: new Uint8Array(png),
			mimeType: "image/png",
		});
	});

	test("X11: uses xclip before the native reader", async () => {
		mocks.spawnSync.mockImplementation((command, args) => {
			expect(command).toBe("xclip");
			return spawnResult(args.includes("TARGETS") ? Buffer.from("image/png\n") : Buffer.from(png));
		});
		expect(await readClipboardImage({ platform: "linux", env: {} })).toEqual({ bytes: png, mimeType: "image/png" });
		expect(mocks.getNativeClipboard).not.toHaveBeenCalled();
	});

	test("X11: an empty clipboard stops fallback without probing every image type", async () => {
		mocks.spawnSync.mockReturnValue(spawnResult(Buffer.from("UTF8_STRING\n")));
		expect(await readClipboardImage({ platform: "linux", env: {} })).toBeNull();
		expect(mocks.spawnSync).toHaveBeenCalledOnce();
		expect(mocks.getNativeClipboard).not.toHaveBeenCalled();
	});

	test("X11: falls back to the native reader when xclip is unavailable", async () => {
		expect(await readClipboardImage({ platform: "linux", env: { DISPLAY: ":0" } })).toEqual({
			bytes: png,
			mimeType: "image/png",
		});
		expect(mocks.getNativeClipboard).toHaveBeenCalledExactlyOnceWith("x11");
		expect(mocks.getImage).toHaveBeenCalledOnce();
	});

	for (const platform of ["darwin", "win32"] as const) {
		test(`${platform}: reads the native clipboard once without command fallbacks`, async () => {
			expect(await readClipboardImage({ platform, env: {} })).toEqual({ bytes: png, mimeType: "image/png" });
			expect(mocks.getImage).toHaveBeenCalledOnce();
			expect(mocks.spawnSync).not.toHaveBeenCalled();
		});
	}

	test("Termux does not read image clipboards", async () => {
		expect(await readClipboardImage({ platform: "linux", env: { TERMUX_VERSION: "0.119" } })).toBeNull();
		expect(mocks.getNativeClipboard).not.toHaveBeenCalled();
		expect(mocks.spawnSync).not.toHaveBeenCalled();
	});
});
