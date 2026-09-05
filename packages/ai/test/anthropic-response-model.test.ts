import type Anthropic from "@anthropic-ai/sdk";
import { describe, expect, it } from "vitest";
import { stream as streamAnthropic } from "../src/api/anthropic-messages.ts";
import { transformMessages } from "../src/api/transform-messages.ts";
import { getModel } from "../src/compat.ts";
import type { Model } from "../src/types.ts";

// Regression test for earendil-works/pi#9188.
// Proxies and relays (new-api, gateways) commonly rewrite the model name in
// the streamed message_start event. `model` must stay pinned to the requested
// id with the echoed name surfaced on `responseModel`, mirroring the
// openai-completions adapter. Overwriting `model` makes transformMessages
// treat the session history as cross-model and downgrade signed thinking
// blocks to plain text on replay, which degrades reasoning-heavy models that
// require their reasoning content to be replayed (e.g. Kimi).

function createSseResponse(events: Array<{ event: string; data: string }>): Response {
	const body = events.map(({ event, data }) => `event: ${event}\ndata: ${data}\n`).join("\n");
	return new Response(body, {
		status: 200,
		headers: { "content-type": "text/event-stream" },
	});
}

function createFakeAnthropicClient(response: Response): Anthropic {
	return {
		beta: {
			messages: {
				create: () => ({
					asResponse: async () => response,
				}),
			},
		},
	} as unknown as Anthropic;
}

function relabeledStreamEvents(echoedModel: string) {
	return [
		{
			event: "message_start",
			data: JSON.stringify({
				type: "message_start",
				message: {
					id: "msg_relabel",
					model: echoedModel,
					usage: { input_tokens: 100, output_tokens: 0 },
				},
			}),
		},
		{
			event: "content_block_start",
			data: JSON.stringify({
				type: "content_block_start",
				index: 0,
				content_block: { type: "thinking", thinking: "" },
			}),
		},
		{
			event: "content_block_delta",
			data: JSON.stringify({
				type: "content_block_delta",
				index: 0,
				delta: { type: "thinking_delta", thinking: "Chain of thought" },
			}),
		},
		{
			event: "content_block_delta",
			data: JSON.stringify({
				type: "content_block_delta",
				index: 0,
				delta: { type: "signature_delta", signature: "sig-abc" },
			}),
		},
		{ event: "content_block_stop", data: JSON.stringify({ type: "content_block_stop", index: 0 }) },
		{
			event: "message_delta",
			data: JSON.stringify({
				type: "message_delta",
				delta: { stop_reason: "end_turn" },
				usage: { input_tokens: 100, output_tokens: 7 },
			}),
		},
		{ event: "message_stop", data: JSON.stringify({ type: "message_stop" }) },
	];
}

describe("anthropic-messages response model echo", () => {
	it("keeps model pinned to the requested id and surfaces the echo on responseModel", async () => {
		const model = getModel("anthropic", "claude-opus-5");
		const result = await streamAnthropic(
			model,
			{ messages: [{ role: "user", content: "Hello", timestamp: 1 }] },
			{ client: createFakeAnthropicClient(createSseResponse(relabeledStreamEvents("kimi-for-coding"))) },
		).result();

		expect(result.model).toBe("claude-opus-5");
		expect(result.responseModel).toBe("kimi-for-coding");
	});

	it("leaves responseModel undefined when the echo matches the requested id", async () => {
		const model = getModel("anthropic", "claude-opus-5");
		const result = await streamAnthropic(
			model,
			{ messages: [{ role: "user", content: "Hello", timestamp: 1 }] },
			{ client: createFakeAnthropicClient(createSseResponse(relabeledStreamEvents("claude-opus-5"))) },
		).result();

		expect(result.model).toBe("claude-opus-5");
		expect(result.responseModel).toBeUndefined();
	});

	it("still attributes cost to an allowed fallback model", async () => {
		const model = {
			...getModel("anthropic", "claude-opus-5"),
			compat: {
				allowedFallbackModels: [
					{
						provider: "anthropic",
						model: "kimi-for-coding",
						cost: { input: 3, output: 3, cacheRead: 0, cacheWrite: 0 },
					},
				],
			},
		} as Model<"anthropic-messages">;
		const result = await streamAnthropic(
			model,
			{ messages: [{ role: "user", content: "Hello", timestamp: 1 }] },
			{ client: createFakeAnthropicClient(createSseResponse(relabeledStreamEvents("kimi-for-coding"))) },
		).result();

		// 100 input tokens at the fallback rate of 3 per million
		expect(result.usage.cost.input).toBeCloseTo((3 / 1_000_000) * 100, 10);
	});

	it("keeps signed thinking blocks replayable when the proxy relabels the model", async () => {
		const model = getModel("anthropic", "claude-opus-5");
		const result = await streamAnthropic(
			model,
			{ messages: [{ role: "user", content: "Hello", timestamp: 1 }] },
			{ client: createFakeAnthropicClient(createSseResponse(relabeledStreamEvents("kimi-for-coding"))) },
		).result();

		const transformed = transformMessages([{ role: "user", content: "Hello", timestamp: 1 }, result], model);
		const assistant = transformed.find((m) => m.role === "assistant");
		const blocks = assistant?.content ?? [];
		const thinking = blocks.filter((b) => b.type === "thinking");
		expect(thinking).toHaveLength(1);
		expect(thinking[0]).toMatchObject({ thinking: "Chain of thought", thinkingSignature: "sig-abc" });
	});

	it("still downgrades thinking for a genuinely different model", async () => {
		const model = getModel("anthropic", "claude-opus-5");
		const result = await streamAnthropic(
			model,
			{ messages: [{ role: "user", content: "Hello", timestamp: 1 }] },
			{ client: createFakeAnthropicClient(createSseResponse(relabeledStreamEvents("kimi-for-coding"))) },
		).result();

		// Simulate the old behavior where the echo overwrote the model id.
		const relabeled = { ...result, model: "kimi-for-coding" };
		const transformed = transformMessages([{ role: "user", content: "Hello", timestamp: 1 }, relabeled], model);
		const assistant = transformed.find((m) => m.role === "assistant");
		const blocks = assistant?.content ?? [];
		const thinking = blocks.filter((b) => b.type === "thinking");
		expect(thinking).toHaveLength(0);
	});
});
