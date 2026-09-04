import assert from "node:assert/strict";
import { test } from "node:test";
import { getNativeClipboard, getNativeClipboardReader } from "../src/native-platform.ts";

// Opt in on a Windows test desktop: this replaces the system clipboard contents.
test(
	"writes Windows clipboard text through the native helper without command fallbacks",
	{ skip: process.platform !== "win32" || process.env.PI_TEST_NATIVE_CLIPBOARD !== "1" },
	() => {
		const clipboard = getNativeClipboard();
		assert.ok(clipboard);
		for (const text of ["clipboard café 日本語", "", "second write"]) {
			clipboard.setText(text);
			assert.equal(clipboard.getText(), text);
		}
	},
);

test(
	"loads the clipboard API from the current native platform helper",
	{ skip: !["darwin", "win32"].includes(process.platform) || !["arm64", "x64"].includes(process.arch) },
	() => {
		const reader = getNativeClipboardReader();
		assert.ok(reader);
		assert.equal(typeof reader.getText, "function");
		assert.equal(typeof reader.hasImage, "function");
		assert.equal(typeof reader.getImageBinary, "function");

		const clipboard = getNativeClipboard();
		assert.ok(clipboard);
		assert.equal(typeof clipboard.setText, "function");
	},
);
