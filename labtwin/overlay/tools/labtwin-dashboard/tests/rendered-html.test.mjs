import assert from "node:assert/strict";
import test from "node:test";

async function render() {
  const workerUrl = new URL("../dist/server/index.js", import.meta.url);
  workerUrl.searchParams.set("test", `${process.pid}-${Date.now()}`);
  const { default: worker } = await import(workerUrl.href);

  return worker.fetch(
    new Request("http://localhost/", {
      headers: { accept: "text/html" },
    }),
    {
      ASSETS: {
        fetch: async () => new Response("Not found", { status: 404 }),
      },
    },
    {
      waitUntil() {},
      passThroughOnException() {},
    },
  );
}

test("server-renders the LabTwin dashboard shell", async () => {
  const response = await render();
  assert.equal(response.status, 200);
  assert.match(response.headers.get("content-type") ?? "", /^text\/html\b/i);

  const html = await response.text();
  assert.match(html, /<title>LabTwin 实验看板<\/title>/i);
  assert.match(html, /实验队列/);
  assert.match(html, /环境状态/);
  assert.match(html, /事实时间线/);
  assert.match(html, /证据链状态/);
  assert.match(html, /同步诊断/);
  assert.match(html, /管理员登录/);
  assert.doesNotMatch(html, /codex-preview|Your site is taking shape/);
});
