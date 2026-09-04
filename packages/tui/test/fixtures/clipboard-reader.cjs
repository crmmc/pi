const { createHash } = require("node:crypto");

try {
	const helper = require(process.argv[2]);
	const value = helper[process.argv[3]]();
	const bytes = typeof value === "boolean" ? undefined : Buffer.from(value);
	console.log(JSON.stringify({
		ok: true,
		value: typeof value === "string" && value.length > 100 ? undefined : typeof value === "object" ? undefined : value,
		length: bytes?.length,
		hash: bytes && createHash("sha256").update(bytes).digest("hex"),
	}));
} catch (error) {
	console.log(JSON.stringify({ ok: false, error: String(error) }));
}
