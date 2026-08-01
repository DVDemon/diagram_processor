#!/usr/bin/env python3
"""
Integration tests for the Diagram Processing Server REST API.

Requires a running server (default http://localhost:8080, override with API_BASE_URL).
Confluence endpoints use real page IDs, overridable via PAGE_ID_1 / PAGE_ID_2.

Run:
    python3 test/api_integration_test.py            # standalone runner (stdlib only)
    pytest test/api_integration_test.py             # pytest runner (same tests)

Useful env vars:
    API_BASE_URL=http://localhost:8080   # server base URL
    PAGE_ID_1=1135648503                 # first Confluence page (PlantUML-heavy)
    PAGE_ID_2=724160182                  # second Confluence page (DrawIO-heavy)
    SKIP_AI=1                            # skip process_with_ai (costs API credits)
    TEST_TIMEOUT=120                     # per-request timeout (seconds)
"""

import json
import os
import subprocess
import sys
import urllib.error
import urllib.request
from typing import Any, Dict, Optional, Tuple

BASE_URL = os.environ.get("API_BASE_URL", "http://localhost:8080").rstrip("/")
PAGE_ID_1 = os.environ.get("PAGE_ID_1", "1135648503")
PAGE_ID_2 = os.environ.get("PAGE_ID_2", "724160182")
SKIP_AI = os.environ.get("SKIP_AI", "0") in ("1", "true", "TRUE")
TIMEOUT = int(os.environ.get("TEST_TIMEOUT", "120"))

try:
    import pytest  # noqa: F401
    _HAS_PYTEST = True
except ImportError:  # pragma: no cover
    _HAS_PYTEST = False

# --------------------------------------------------------------------------- #
# HTTP helpers (stdlib only)
# --------------------------------------------------------------------------- #

def http(method, path, body=None, timeout=TIMEOUT,
         content_type=None) -> Tuple[int, Optional[Dict[str, Any]], str]:
    """Return (status, parsed_json_or_None, raw_text). Raise on connect error."""
    url = BASE_URL + path
    data = None
    headers = {}
    if body is not None:
        if isinstance(body, dict):
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = content_type or "application/json"
        else:
            data = body.encode("utf-8")
            headers["Content-Type"] = content_type or "text/plain"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", "replace")
            return resp.status, _maybe_json(raw), raw
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        return e.code, _maybe_json(raw), raw
    except urllib.error.URLError as e:
        raise AssertionError(
            f"Cannot reach server at {BASE_URL}: {e}. "
            "Start it first: docker-compose up -d --build"
        )


def http_json(method, path, body=None, timeout=TIMEOUT, expected=200,
              content_type=None) -> Dict[str, Any]:
    """Like http(), but asserts the expected status (int or tuple) and a JSON body."""
    status, data, raw = http(method, path, body, timeout, content_type)
    ok = (status == expected) if isinstance(expected, int) else (status in expected)
    assert ok, f"{method} {path} -> HTTP {status}, expected {expected}: {raw[:300]}"
    assert data is not None, f"non-JSON response for {method} {path}: {raw[:300]}"
    return data


def _poll_ai_result(request_id: int, timeout: float = 60.0, interval: float = 0.5) -> Dict[str, Any]:
    """Опрашивает /async_ai_result до тех пор, пока статус не перестанет быть running."""
    import time
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        data = http_json("GET", f"/api/v1/async_ai_result?request_id={request_id}",
                         expected=(200, 202))
        if data.get("status") != "running":
            return data
        time.sleep(interval)
    raise AssertionError(f"async job {request_id} did not finish within {timeout}s")


def _maybe_json(raw) -> Optional[Dict[str, Any]]:
    try:
        return json.loads(raw)
    except Exception:
        return None


def _skip(msg):
    """Skip a test (pytest-aware, otherwise a no-op returning None)."""
    if _HAS_PYTEST:
        pytest.skip(msg)
    else:
        print(f"  SKIP  {msg}")


# --------------------------------------------------------------------------- #
# Parser fixtures (same as the API documentation examples)
# --------------------------------------------------------------------------- #

SEQ_FIXTURE = """@startuml
participant "User" as User
participant "API" as API
participant "Database" as DB
User -> API : Login request
API -> DB : Query user
DB --> API : User data
API --> User : Auth token
@enduml"""

C4_FIXTURE = """@startuml
rectangle "==My System" <<SoftwareSystem>> as sys1
rectangle "==External API" <<Container>> as ext1
sys1 .[#red].> ext1 : "REST API"
@enduml"""

DRAWIO_FIXTURE = (
    '<mxfile host="test"><diagram><mxGraphModel><root>'
    '<mxCell id="0"/><mxCell id="1" parent="0"/>'
    '<object id="sys1" c4Name="Backend" c4Type="SoftwareSystem">'
    '<mxCell parent="1"/><mxGeometry x="20" y="20" width="120" height="60" as="geometry"/></object>'
    '<object id="cont1" c4Name="API" c4Type="Container">'
    '<mxCell parent="1"/><mxGeometry x="200" y="20" width="120" height="60" as="geometry"/></object>'
    '<object id="rel1" c4Type="Relationship" c4Description="Calls">'
    '<mxCell parent="1" source="sys1" target="cont1"/></object>'
    '</root></mxGraphModel></diagram></mxfile>'
)


# --------------------------------------------------------------------------- #
# Tests
# --------------------------------------------------------------------------- #

def test_server_up():
    status, _, _ = http("GET", "/metrics")
    assert status == 200, f"GET /metrics -> {status}"


def test_swagger():
    status, _, raw = http("GET", "/swagger.yaml")
    assert status == 200, f"GET /swagger.yaml -> {status}"
    assert "openapi" in raw.lower(), "swagger.yaml does not look like OpenAPI"


def test_metrics():
    status, _, raw = http("GET", "/metrics")
    assert status == 200, f"GET /metrics -> {status}"
    assert "http_requests_total" in raw, "missing http_requests_total metric"
    assert "http_request_duration_seconds" in raw, "missing duration histogram"


# --- process_with_ai ------------------------------------------------------- #

def test_process_with_ai():
    if SKIP_AI:
        _skip("process_with_ai skipped (SKIP_AI=1)")
        return
    data = http_json("POST", "/api/v1/process_with_ai", {"text": "Ответь одним словом: 2+2?"})
    assert data.get("success") is True
    assert data.get("result"), "empty AI result"


def test_process_with_ai_missing_text():
    data = http_json("POST", "/api/v1/process_with_ai", {}, expected=400)
    assert "text" in data.get("error", "")


def test_process_with_ai_invalid_json():
    data = http_json("POST", "/api/v1/process_with_ai", "not json{{{",
                     content_type="application/json", expected=400)
    assert data.get("success") is False


# --- async AI (process_with_ai_async / async_ai_status / async_ai_result) --- #

def test_process_with_ai_async():
    if SKIP_AI:
        _skip("async AI skipped (SKIP_AI=1)")
        return
    data = http_json("POST", "/api/v1/process_with_ai_async", {"text": "Ответь одним словом: 2+2?"},
                     expected=202)
    rid = data.get("request_id")
    assert isinstance(rid, int) and rid > 0, f"bad request_id: {rid}"
    assert data.get("status") == "running"

    # Статусный эндпоинт должен отвечать в любом состоянии
    st = http_json("GET", f"/api/v1/async_ai_status?request_id={rid}")
    assert st.get("status") in ("running", "completed", "failed")
    assert "start_time_ms" in st
    assert "retries" in st and "bytes_sent" in st

    # Дожидаемся завершения и берём результат
    res = _poll_ai_result(rid)
    assert res.get("status") == "completed", f"unexpected: {res}"
    assert res.get("result"), "empty async result"


def test_process_with_ai_async_missing_text():
    data = http_json("POST", "/api/v1/process_with_ai_async", {}, expected=400)
    assert "text" in data.get("error", "")


def test_async_ai_status_missing_request_id():
    data = http_json("GET", "/api/v1/async_ai_status", expected=400)
    assert "request_id" in data.get("error", "")


def test_async_ai_status_invalid_request_id():
    data = http_json("GET", "/api/v1/async_ai_status?request_id=abc", expected=400)
    assert "integer" in data.get("error", "")


def test_async_ai_status_not_found():
    data = http_json("GET", "/api/v1/async_ai_status?request_id=99999999", expected=404)
    assert "not found" in data.get("error", "")


def test_async_ai_result_not_found():
    data = http_json("GET", "/api/v1/async_ai_result?request_id=99999999", expected=404)
    assert "not found" in data.get("error", "")


# --- parse_plantuml_sequence ----------------------------------------------- #

def test_parse_plantuml_sequence():
    data = http_json("POST", "/api/v1/parse_plantuml_sequence", {"text": SEQ_FIXTURE})
    assert data.get("success") is True
    comps = data.get("components", [])
    reqs = data.get("requests", [])
    assert len(comps) >= 3, f"expected >=3 components, got {len(comps)}"
    assert len(reqs) >= 3, f"expected >=3 requests, got {len(reqs)}"
    # каждый запрос ссылается на существующие компоненты
    ids = {c["id"] for c in comps}
    for r in reqs:
        assert r["component_source_id"] in ids, f"unknown source {r['component_source_id']}"
        assert r["component_target_id"] in ids, f"unknown target {r['component_target_id']}"


def test_parse_plantuml_sequence_not_sequence():
    data = http_json("POST", "/api/v1/parse_plantuml_sequence", {"text": "hello world"}, expected=400)
    assert data.get("success") is False


# --- parse_plantuml_c4 ------------------------------------------------------ #

def test_parse_plantuml_c4():
    data = http_json("POST", "/api/v1/parse_plantuml_c4", {"text": C4_FIXTURE})
    assert data.get("success") is True
    assert len(data.get("components", [])) >= 2
    assert len(data.get("requests", [])) >= 1
    types = {c["c4_type"] for c in data["components"]}
    assert "SoftwareSystem" in types, f"expected SoftwareSystem in types, got {types}"


def test_parse_plantuml_c4_not_c4():
    data = http_json("POST", "/api/v1/parse_plantuml_c4", {"text": "hello world"}, expected=400)
    assert data.get("success") is False


# --- parse_drawio ----------------------------------------------------------- #

def test_parse_drawio():
    data = http_json("POST", "/api/v1/parse_drawio", {"text": DRAWIO_FIXTURE})
    assert data.get("success") is True
    assert len(data.get("components", [])) >= 2
    assert len(data.get("requests", [])) >= 1
    assert isinstance(data.get("parent_child"), list)


def test_parse_drawio_invalid():
    data = http_json("POST", "/api/v1/parse_drawio", {"text": "<not-drawio/>"}, expected=400)
    assert data.get("success") is False


def test_parse_drawio_empty():
    data = http_json("POST", "/api/v1/parse_drawio", {"text": ""}, expected=400)
    assert data.get("success") is False


# --- load_confluence -------------------------------------------------------- #

def test_load_confluence_page1():
    _check_load_page(PAGE_ID_1)


def test_load_confluence_page2():
    _check_load_page(PAGE_ID_2)


def _check_load_page(page_id):
    data = http_json("GET", f"/api/v1/load_confluence?page_id={page_id}")
    assert data.get("page_id") == page_id
    assert data.get("html"), "empty html"
    assert isinstance(data.get("children"), list)


def test_load_confluence_include_subpages():
    data = http_json("GET", f"/api/v1/load_confluence?page_id={PAGE_ID_1}&include_subpages=1")
    assert data.get("page_id") == PAGE_ID_1
    assert isinstance(data.get("children"), list)


def test_load_confluence_missing_page_id():
    data = http_json("GET", "/api/v1/load_confluence", expected=400)
    assert "page_id" in data.get("error", "")


# --- parse_confluence ------------------------------------------------------- #

def test_parse_confluence_page1():
    data = http_json("GET", f"/api/v1/parse_confluence?page_id={PAGE_ID_1}")
    _assert_diagram_list(data)
    # Страница 1 богата PlantUML-диаграммами
    formats = {d["format"] for d in data["diagrams"]}
    assert "plantuml" in formats, f"expected plantuml diagrams, got formats {formats}"


def test_parse_confluence_page2():
    data = http_json("GET", f"/api/v1/parse_confluence?page_id={PAGE_ID_2}")
    _assert_diagram_list(data)
    # Страница 2 богата DrawIO-диаграммами
    formats = {d["format"] for d in data["diagrams"]}
    assert "drawio" in formats, f"expected drawio diagrams, got formats {formats}"


def test_parse_confluence_pipeline_drawio():
    """Пайплайн: parse_confluence → XML drawio → parse_drawio."""
    data = http_json("GET", f"/api/v1/parse_confluence?page_id={PAGE_ID_2}")
    drawio_diagrams = [d for d in data.get("diagrams", []) if d["format"] == "drawio"]
    assert drawio_diagrams, "page 2 should contain drawio diagrams"
    # Первая drawio-диаграмма должна разбираться парсером
    parsed = http_json("POST", "/api/v1/parse_drawio", {"text": drawio_diagrams[0]["text"]})
    assert parsed.get("success") is True


def test_parse_confluence_missing_page_id():
    data = http_json("GET", "/api/v1/parse_confluence", expected=400)
    assert "page_id" in data.get("error", "")


def _assert_diagram_list(data):
    count = data.get("count")
    diagrams = data.get("diagrams")
    assert isinstance(count, int) and count >= 1, f"expected count>=1, got {count}"
    assert isinstance(diagrams, list) and len(diagrams) == count
    for d in diagrams:
        assert d.get("text"), "diagram without text"
        assert d.get("format") in ("plantuml", "drawio"), f"bad format {d.get('format')}"
        assert "source_page_id" in d, "diagram without source_page_id"


# --- HTTP plumbing ---------------------------------------------------------- #

def test_not_found():
    status, _, _ = http("GET", "/api/v1/definitely_not_here")
    assert status == 404, f"expected 404, got {status}"


def test_wrong_method_returns_not_found():
    # Маршрутизатор диспатчит только по точному URI+методу → GET на POST-эндпоинте = 404
    status, _, _ = http("GET", "/api/v1/process_with_ai")
    assert status == 404, f"expected 404, got {status}"


# --- json2dot utility ------------------------------------------------------- #

def test_json2dot():
    script = os.path.join(os.path.dirname(__file__), "..", "scripts", "json2dot.py")
    if not os.path.exists(script):
        _skip("scripts/json2dot.py not found")
        return
    sample = {
        "success": True,
        "components": [{"id": "a", "code": "a", "name": "A", "c4_type": "SoftwareSystem"}],
        "requests": [{"request_id": 1, "component_source_id": "a",
                      "component_target_id": "b", "description": "x"}],
        "parent_child": [],
    }
    proc = subprocess.run(
        [sys.executable, script], input=json.dumps(sample),
        capture_output=True, text=True, timeout=30,
    )
    assert proc.returncode == 0, f"json2dot failed: {proc.stderr}"
    assert "digraph" in proc.stdout


# --------------------------------------------------------------------------- #
# Standalone runner
# --------------------------------------------------------------------------- #

def _ordered_tests():
    return [
        ("server up (GET /metrics)", test_server_up),
        ("swagger.yaml", test_swagger),
        ("metrics", test_metrics),
        ("process_with_ai", test_process_with_ai),
        ("process_with_ai: missing text -> 400", test_process_with_ai_missing_text),
        ("process_with_ai: invalid JSON -> 400", test_process_with_ai_invalid_json),
        ("process_with_ai_async (submit→status→result)", test_process_with_ai_async),
        ("process_with_ai_async: missing text -> 400", test_process_with_ai_async_missing_text),
        ("async_ai_status: missing request_id -> 400", test_async_ai_status_missing_request_id),
        ("async_ai_status: invalid request_id -> 400", test_async_ai_status_invalid_request_id),
        ("async_ai_status: unknown id -> 404", test_async_ai_status_not_found),
        ("async_ai_result: unknown id -> 404", test_async_ai_result_not_found),
        ("parse_plantuml_sequence", test_parse_plantuml_sequence),
        ("parse_plantuml_sequence: not a sequence -> 400", test_parse_plantuml_sequence_not_sequence),
        ("parse_plantuml_c4", test_parse_plantuml_c4),
        ("parse_plantuml_c4: not C4 -> 400", test_parse_plantuml_c4_not_c4),
        ("parse_drawio", test_parse_drawio),
        ("parse_drawio: invalid -> 400", test_parse_drawio_invalid),
        ("parse_drawio: empty -> 400", test_parse_drawio_empty),
        (f"load_confluence page {PAGE_ID_1}", test_load_confluence_page1),
        (f"load_confluence page {PAGE_ID_2}", test_load_confluence_page2),
        ("load_confluence include_subpages=1", test_load_confluence_include_subpages),
        ("load_confluence: missing page_id -> 400", test_load_confluence_missing_page_id),
        (f"parse_confluence page {PAGE_ID_1} (plantuml)", test_parse_confluence_page1),
        (f"parse_confluence page {PAGE_ID_2} (drawio)", test_parse_confluence_page2),
        ("parse_confluence → parse_drawio pipeline", test_parse_confluence_pipeline_drawio),
        ("parse_confluence: missing page_id -> 400", test_parse_confluence_missing_page_id),
        ("unknown path -> 404", test_not_found),
        ("wrong method -> 404", test_wrong_method_returns_not_found),
        ("json2dot utility", test_json2dot),
    ]


def main():
    import traceback

    # Быстрая проверка, что сервер поднят, до запуска всей пачки
    try:
        test_server_up()
    except AssertionError as e:
        print(f"ERROR: server is not reachable: {e}")
        sys.exit(2)

    tests = _ordered_tests()
    passed, failed = 0, 0
    print(f"API base URL : {BASE_URL}")
    print(f"Confluence   : PAGE_ID_1={PAGE_ID_1}, PAGE_ID_2={PAGE_ID_2}, "
          f"SKIP_AI={SKIP_AI}")
    print(f"Running {len(tests)} tests...\n")
    for name, fn in tests:
        try:
            fn()
            passed += 1
            print(f"  PASS  {name}")
        except AssertionError as e:
            failed += 1
            print(f"  FAIL  {name}: {e}")
        except Exception as e:  # pragma: no cover
            failed += 1
            print(f"  ERROR {name}: {e}")
            traceback.print_exc()
    print(f"\nResult: {passed}/{len(tests)} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
