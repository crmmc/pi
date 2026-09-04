import type * as Tui from "@earendil-works/pi-tui";
import { afterEach, beforeEach, describe, expect, test, vi } from "vitest";
import { InteractiveMode } from "../src/modes/interactive/interactive-mode.ts";
import { readClipboardImage } from "../src/utils/clipboard-image.ts";

const mocks = vi.hoisted(() => ({
	available: true,
	getImage: vi.fn<() => Uint8Array | null>(),
	readClipboardText: vi.fn<() => Promise<string | null>>(),
}));

vi.mock("@earendil-works/pi-tui", async (importOriginal) => ({
	...(await importOriginal<typeof Tui>()),
	getNativeClipboard: () => (mocks.available ? mocks : undefined),
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
		vi.resetAllMocks();
		vi.stubEnv("TERMUX_VERSION", "");
		mocks.available = true;
	});

	afterEach(() => vi.unstubAllEnvs());

	test("preserves explicit no-image results", async () => {
		mocks.available = false;
		expect(await readClipboardImage({ platform: "win32", env: {} })).toBeNull();
		expect(mocks.getImage).not.toHaveBeenCalled();
		mocks.available = true;
		for (const empty of [null, new Uint8Array()]) {
			mocks.getImage.mockReturnValue(empty);
			expect(await readClipboardImage({ platform: "win32", env: {} })).toBeNull();
		}
	});

	test("propagates read errors and aborts paste without reading text", async () => {
		const error = new Error("Native clipboard operation failed");
		mocks.getImage.mockImplementation(() => {
			throw error;
		});
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
});
