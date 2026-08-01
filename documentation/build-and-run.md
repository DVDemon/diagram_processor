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
