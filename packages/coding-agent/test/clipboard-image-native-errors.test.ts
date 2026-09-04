import { afterEach, beforeEach, describe, expect, test, vi } from "vitest";
import { InteractiveMode } from "../src/modes/interactive/interactive-mode.ts";
import { readClipboardImage } from "../src/utils/clipboard-image.ts";

const mocks = vi.hoisted(() => ({
	available: true,
	hasImage: vi.fn<() => boolean>(),
	getImageBinary: vi.fn<() => Uint8Array | null | Promise<Uint8Array | null>>(),
	readClipboardText: vi.fn<() => Promise<string | null>>(),
}));

vi.mock("../src/utils/clipboard-native.ts", () => ({
	getClipboardReader: () => (mocks.available ? mocks : null),
}));
vi.mock("../src/utils/clipboard.ts", () => ({
	copyToClipboard: vi.fn(),
	readClipboardText: mocks.readClipboardText,
}));
vi.mock("child_process", () => ({
	spawnSync: vi.fn(() => ({ status: 1, stdout: Buffer.alloc(0) })),
}));

describe("native clipboard image failures", () => {
	beforeEach(() => {
		vi.clearAllMocks();
		vi.stubEnv("TERMUX_VERSION", "");
		mocks.available = true;
		mocks.hasImage.mockReset().mockReturnValue(true);
		mocks.getImageBinary.mockReset();
	});

	afterEach(() => vi.unstubAllEnvs());

	test("preserves explicit no-image results", async () => {
		mocks.available = false;
		expect(await readClipboardImage({ platform: "win32", env: {} })).toBeNull();
		mocks.available = true;
		mocks.hasImage.mockReturnValue(false);
		expect(await readClipboardImage({ platform: "win32", env: {} })).toBeNull();
		expect(mocks.getImageBinary).not.toHaveBeenCalled();
		mocks.hasImage.mockReturnValue(true);
		for (const empty of [null, new Uint8Array()]) {
			mocks.getImageBinary.mockReturnValue(empty);
			expect(await readClipboardImage({ platform: "win32", env: {} })).toBeNull();
		}
	});

	for (const failure of ["probe", "read", "async read"] as const) {
		test(`propagates ${failure} errors and aborts paste without reading text`, async () => {
			const error = new Error("Native clipboard operation failed");
			if (failure === "probe") {
				mocks.hasImage.mockImplementation(() => {
					throw error;
				});
			} else if (failure === "read") {
				mocks.getImageBinary.mockImplementation(() => {
					throw error;
				});
			} else {
				mocks.getImageBinary.mockRejectedValue(error);
			}

			await expect(readClipboardImage({ platform: "win32", env: {} })).rejects.toBe(error);
			const context = {
				editor: { insertTextAtCursor: vi.fn() },
				ui: { requestRender: vi.fn() },
			};
			const prototype = InteractiveMode.prototype as unknown as {
				handleClipboardPaste(this: typeof context): Promise<void>;
			};
			// The interaction boundary contains the failure without changing editor contents.
			await prototype.handleClipboardPaste.call(context);
			expect(mocks.readClipboardText).not.toHaveBeenCalled();
			expect(context.editor.insertTextAtCursor).not.toHaveBeenCalled();
			expect(context.ui.requestRender).not.toHaveBeenCalled();
		});
	}
});
