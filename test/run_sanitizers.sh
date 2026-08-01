#!/usr/bin/env bash
# Runs unit tests and a server smoke-test under AddressSanitizer / UndefinedBehaviorSanitizer.
# Expected to be executed from /build/app/build inside the Dockerfile.test image.
set -uo pipefail

FAIL=0
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1:abort_on_error=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"

# Workaround for ASan on kernels with high ASLR entropy (vm.mmap_rnd_bits=32),
# which fails with "unexpected memory mapping" on some Linux 6.x hosts.
# Если setarch недоступен/запрещён — просто выполняем команду без него.
san_prefix() {
    if command -v setarch >/dev/null 2>&1 && setarch "$(uname -m)" -R true 2>/dev/null; then
        echo "setarch $(uname -m) -R"
    fi
}

echo "=== 1/3 Unit tests (parser_tests) — ASan/UBSan ==="
# shellcheck disable=SC2086
if ! $(san_prefix) ./parser_tests; then
    echo "FAIL: unit tests"
    FAIL=1
fi

echo
echo "=== 2/3 Server smoke test — ASan/UBSan ==="
LOG=/tmp/server_sanitizers.log
: > "$LOG"
# Простой командой (не функцией) — тогда $! указывает на реальный PID сервера.
# shellcheck disable=SC2086
$(san_prefix) ./poco_template_server > "$LOG" 2>&1 &
SERVER_PID=$!

READY=0
for _ in $(seq 1 30); do
    if curl -sf -o /dev/null http://localhost:8080/metrics 2>/dev/null; then
        READY=1
        break
    fi
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        break
    fi
    sleep 1
done
if [ "$READY" != "1" ]; then
    echo "FAIL: server did not become ready"
    cat "$LOG"
    kill -KILL "$SERVER_PID" 2>/dev/null || true
    exit 1
fi

echo "-- /metrics --"
curl -s http://localhost:8080/metrics | grep -a http_requests_total | head -1 || true

echo "-- /api/v1/parse_plantuml_sequence --"
curl -s -X POST http://localhost:8080/api/v1/parse_plantuml_sequence \
  -H 'Content-Type: application/json' \
  -d '{"text":"@startuml\nparticipant User\nparticipant API\nUser -> API : Login\nAPI --> User : Token\n@enduml"}' \
  | head -c 300 || true
echo

echo "-- /api/v1/parse_drawio --"
curl -s -X POST http://localhost:8080/api/v1/parse_drawio \
  -H 'Content-Type: application/json' \
  -d '{"text":"<mxfile><diagram><mxGraphModel><root><mxCell id=\"0\"/><mxCell id=\"1\" parent=\"0\"/><object id=\"a\" c4Name=\"A\" c4Type=\"SoftwareSystem\"><mxCell parent=\"1\"/><mxGeometry x=\"0\" y=\"0\" width=\"100\" height=\"50\" as=\"geometry\"/></object></root></mxGraphModel></diagram></mxfile>"}' \
  | head -c 200 || true
echo

echo "-- /api/v1/process_with_ai (no key -> 503) --"
curl -s -o /dev/null -w "HTTP %{http_code}\n" -X POST http://localhost:8080/api/v1/process_with_ai \
  -H 'Content-Type: application/json' -d '{"text":"hi"}' || true

echo "-- /api/v1/parse_confluence (no CONFLUENCE_URL -> 503) --"
curl -s -o /dev/null -w "HTTP %{http_code}\n" "http://localhost:8080/api/v1/parse_confluence?page_id=1" || true

# Останавливаем сервер: SIGTERM (POCO завершается штатно -> LeakSanitizer отрабатывает),
# при зависании — SIGKILL.
kill -TERM "$SERVER_PID" 2>/dev/null || true
for _ in $(seq 1 10); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        break
    fi
    sleep 0.5
done
kill -KILL "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true

echo
echo "=== 3/3 Sanitizer report ==="
if grep -aE "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:" "$LOG"; then
    echo "FAIL: sanitizer errors found in server log"
    cat "$LOG"
    FAIL=1
else
    echo "OK: no sanitizer errors in server log"
fi

echo
if [ "$FAIL" != "0" ]; then
    echo "SANITIZER RUN FAILED"
    exit 1
fi
echo "ALL SANITIZER CHECKS PASSED"
