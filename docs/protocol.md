# Protocol — OpenAI-Compatible API

_Phase 1 stub — full spec lands Phase 5._

## Endpoints

```
GET  /health          → {status, uptime_s, heap, wifi_connected, ip}
GET  /v1/models       → {object:"list", data:[{id, owned_by}]}
POST /v1/chat/completions → proxied to upstream
GET  /admin/status    (auth if LOCAL_API_TOKEN set)
GET  /admin/providers (auth)
GET  /admin/stats     (auth)
```

All responses JSON `Content-Type: application/json`.

Streaming `POST` with `{"stream":true}` returns `Content-Type: text/event-stream` chunked.
