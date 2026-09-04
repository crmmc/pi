const { createHash } = require("node:crypto");

try {
	const helper = require(process.argv[2]);
	const value = helper[process.argv[3]]();
	const bytes = typeof value === "string" || Buffer.isBuffer(value) ? Buffer.from(value) : undefined;
	console.log(JSON.stringify({
		ok: true,
		value: value === null || typeof value === "boolean" || (typeof value === "string" && value.length <= 100) ? value : undefined,
		length: bytes?.length,
		hash: bytes && createHash("sha256").update(bytes).digest("hex"),
	}));
} catch (error) {
	console.log(JSON.stringify({ ok: false, error: String(error) }));
}
