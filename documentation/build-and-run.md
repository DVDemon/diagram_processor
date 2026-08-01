# Сборка и запуск

## Требования

- C++17-компилятор (GCC/Clang), CMake ≥ 3.16
- Библиотеки POCO (Net, NetSSL, Foundation, Util, **Prometheus**, JSON) — версии 1.12+ (Prometheus-модуль появился в POCO 1.12)
- zlib
- Google Test (подтягивается автоматически через `FetchContent` при сборке тестов)

## Локальная сборка

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

- Исполняемый файл: `build/poco_template_server`.
- `install(TARGETS … DESTINATION bin)` — при `cmake --install`.

## Запуск

```bash
cp .env_example .env     # задать настройки
set -a && source .env && set +a
./build/poco_template_server
```

Либо без env-файла (тогда AI/Confluence-эндпоинты вернут `503`):

```bash
PORT=8080 ./build/poco_template_server
```

Проверка:

```bash
curl localhost:8080/metrics
curl localhost:8080/swagger.yaml
```

## Сборка и запуск тестов

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)          # соберёт и parser_tests
cd build && ctest --output-on-failure   # или ./parser_tests
```

Тесты (`test/parser_test.cpp`, Google Test) сверяют вывод парсеров с эталонными JSON в `test/fixtures/`:

- PlantUML Sequence: `simple_sequence`, `arrows_only`, `large_sequence`
- PlantUML C4: `rectangle_c4`, `c4_lib`
- DrawIO: `simple_c4`, `multi_component`, `complex`

### Санитайзерные тесты (`Dockerfile.test`, ASan/UBSan)

`Dockerfile.test` собирает сервер и тесты с `-fsanitize=address,undefined` и запускает: 1) `parser_tests`, 2) smoke-тест сервера (metrics + парсеры + обработка ошибок). Логи проверяются на `ERROR: AddressSanitizer` / `runtime error:`; завершается с ошибкой при любом срабатывании.

```bash
docker build -f Dockerfile.test -t poco_ai_server_test .
docker run --rm poco_ai_server_test      # exit 0 = все санитайзерные проверки пройдены
```

POCO при этом собирается без санитайзеров (инструментируется только наш код); локальная санитайзер-сборка — через `build-san`, см. README.

### Интеграционные тесты API (`test/api_integration_test.py`)

Проверяют **все HTTP-эндпоинты** против запущенного сервера и реального Confluence (по умолчанию страницы `1135648503` и `724160182`). Требуют поднятый сервер и настроенный `CONFLUENCE_*` в `.env`.

```bash
# сервер должен быть запущен
docker-compose up -d --build

# standalone-раннер (только stdlib, без зависимостей)
python3 test/api_integration_test.py

# или под pytest
python3 -m pytest test/api_integration_test.py -v
```

Полезные переменные окружения:

| Переменная | По умолчанию | Описание |
|---|---|---|
| `API_BASE_URL` | `http://localhost:8080` | базовый URL сервера |
| `PAGE_ID_1` | `1135648503` | страница с PlantUML-диаграммами |
| `PAGE_ID_2` | `724160182` | страница с DrawIO-диаграммами |
| `SKIP_AI=1` | выкл | пропустить `process_with_ai` (реальный вызов API, расходует кредиты) |
| `TEST_TIMEOUT` | `120` | таймаут запроса, сек |

Что покрывается: `process_with_ai` (успех + ошибки), асинхронный AI (`process_with_ai_async` → `async_ai_status` → `async_ai_result`, включая ошибки `400`/`404`), все три парсера (валидные/невалидные входы), `load_confluence` и `parse_confluence` (обе страницы, `include_subpages`, ошибка без `page_id`), пайплайн `parse_confluence → parse_drawio`, `metrics`, `swagger.yaml`, `404`/`400`, утилита `json2dot.py`.
- Confluence-парсер: `mixed_diagrams`, `plantuml_only`
- JSON-разбор Confluence-клиента: `extractChildPageIds` (v2-формат, server-обёртка, пустые результаты)

> При сборке через CMake путь к fixtures передаётся как `TEST_FIXTURES_PATH`, поэтому тесты должны запускаться из каталога сборки.

## Docker

### Продакшен-образ (`Dockerfile`)

Двухстадийная сборка:

1. **builder** (ubuntu:24.04): собирает POCO 1.15.0 из исходников (с Prometheus), затем приложение.
2. **runner**: копирует бинарник и `libPoco*.so*`, задаёт `LD_LIBRARY_PATH=/usr/local/lib`, пробрасывает переменные окружения как ENV с теми же значениями по умолчанию.

```bash
docker build -t poco-ai-server .
docker run --rm -p 8080:8080 \
  -e OPENAI_API_KEY=... \
  -e CONFLUENCE_URL=... -e CONFLUENCE_USER=... -e CONFLUENCE_TOKEN=... \
  poco-ai-server
```

### docker-compose (`docker-compose.yaml`)

```bash
docker compose up --build
```

- Порт: `"${PORT:-8080}:${PORT:-8080}"`.
- Все переменные из `.env` прокидываются через `env_file: .env` + блок `environment`.
- Чтобы отключить проверку SSL в Dockerfile по умолчанию — `CONFLUENCE_SSL_VERIFY` задаётся в ENV (в Dockerfile по умолчанию `true`).

### Образ для тестов (`Dockerfile.test`)

Собирает POCO и приложение, запускает `./parser_tests`:

```bash
docker build -f Dockerfile.test -t poco-ai-server-tests .
docker run --rm poco-ai-server-tests
```

## CI/CD

`.github/workflows/cmake-multi-platform.yml`:

- **build** (ubuntu-latest, GCC): сборка POCO, конфигурация CMake, `cmake --build`, `ctest`.
- **docker** (после build, только для `push` в `main`): сборка и публикация мульти-архитектурного образа (`linux/amd64`, `linux/arm64`) в GHCR (`ghcr.io/{owner}/{repo}`, теги `latest` + `sha-{hash}`), кэш через Buildx GH Cache.

## Postman

Коллекция `postman/Poco Template Server.postman_collection.json` содержит готовые примеры всех эндпоинтов — импортируйте в Postman.
