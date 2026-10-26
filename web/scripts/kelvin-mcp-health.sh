#!/bin/sh
# Restart the public Kelvin MCP when /health stops answering. Run every minute by
# kelvin-mcp-health.timer. Two failures in a row (20 s apart; a cold shard load blocks the server ~20 s) before acting, so one slow moment
# during a heavy search does not bounce the service.
ok() { curl -fsS -m 30 http://127.0.0.1:8412/health >/dev/null 2>&1; }
ok && exit 0
sleep 20
ok && exit 0
logger -t kelvin-mcp-health "health check failed twice; restarting kelvin-mcp-public"
systemctl restart kelvin-mcp-public
