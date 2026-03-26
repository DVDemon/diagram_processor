# Diagram Processing Server

REST-сервер на C++ (POCO) для разбора архитектурных диаграмм и интеграции с AI и Confluence.

## Описание системы

Сервер предоставляет единый API для:

- **Парсинг диаграмм** — извлечение компонентов и связей из PlantUML (Sequence, C4) и DrawIO (C4). Результат в формате JSON: `components`, `requests`, `parent_child`.
- **AI-обработка** — отправка текста в DeepSeek/OpenAI и получение ответа.
- **Confluence** — загрузка страниц и извлечение встроенных PlantUML/DrawIO диаграмм из HTML.
- **Метрики** — Prometheus-совместимые метрики и OpenAPI-спецификация (Swagger).

Парсеры поддерживают синхронные/асинхронные взаимодействия, partition, loop, ветвления. JSON-результат можно конвертировать в DOT (Graphviz) для визуализации.

## Структура каталогов

```
poco_ai_server/
├── src/
│   ├── main.cpp                 # Точка входа, HTTP-сервер
│   ├── handler/                 # HTTP-обработчики
│   │   ├── router_factory.h     # Маршрутизация запросов
│   │   ├── parse_plantuml_sequence_handler.h
│   │   ├── parse_plantuml_c4_handler.h
│   │   ├── parse_drawio_handler.h
│   │   ├── parse_confluence_handler.h
│   │   ├── load_confluence_handler.h
│   │   ├── process_with_ai_handler.h
│   │   ├── swagger_handler.h
│   │   └── metrics_handler.h
│   ├── plantuml/                # Парсеры PlantUML
│   │   ├── plantuml_sequence.h
│   │   └── plantuml_c4.h
│   ├── drawio/                  # Парсер DrawIO C4
│   │   └── drawio_parser.h
│   ├── confluence/              # Confluence API и парсер HTML
│   │   ├── confluence_client.h
│   │   └── confluence_parser.h
│   └── openai/                  # Клиент OpenAI/DeepSeek API
│       └── openai_client.h
├── test/
│   ├── parser_test.cpp         # Unit-тесты парсеров (Google Test)
│   └── fixtures/                # Тестовые данные
│       ├── plantuml_sequence/   # .puml + .json
│       ├── plantuml_c4/
│       ├── drawio/
│       └── confluence/
├── scripts/
│   └── json2dot.py              # Конвертация JSON → DOT (Graphviz)
├── postman/                     # Postman-коллекция для API
├── CMakeLists.txt
├── Dockerfile
├── Dockerfile.test
├── docker-compose.yaml
└── .env_example
```

## Endpoints

- `POST /api/v1/process_with_ai` — обработка текста через AI (DeepSeek/OpenAI). JSON: `{ "text" | "prompt" | "message": "..." }`
- `POST /api/v1/parse_plantuml_sequence` — разбор PlantUML Sequence. JSON: `{ "text": "..." }`
- `POST /api/v1/parse_plantuml_c4` — разбор PlantUML C4. JSON: `{ "text": "..." }`
- `POST /api/v1/parse_drawio` — разбор DrawIO C4. JSON: `{ "text" | "xml" | "content" | "drawio": "..." }` (XML диаграммы)
- `GET /api/v1/load_confluence?page_id={id}&include_subpages=0|1` — загрузка страницы из Confluence (HTML)
- `GET /api/v1/parse_confluence?page_id={id}&include_subpages=0|1` — загрузка и разбор страницы (извлечение PlantUML и DrawIO диаграмм)
- `GET /metrics` — метрики Prometheus
- `GET /swagger.yaml` — OpenAPI спецификация в формате YAML

## Переменные окружения

- `PORT` — порт сервера (по умолчанию: 8080)
- `LOG_LEVEL` — уровень логирования: trace, debug, information, notice, warning, error, critical, fatal, none
- `OPENAI_API_KEY` — API ключ для DeepSeek/OpenAI (обязательно для process_with_ai)
- `OPENAI_API_URL` — URL API (по умолчанию: https://api.deepseek.com)
- `OPENAI_MODEL` — модель (по умолчанию: deepseek-chat)
- `OPENAI_SYSTEM_PROMPT` — системный промпт для AI
- `OPENAI_TIMEOUT` — таймаут в секундах (по умолчанию: 60)
- `OPENAI_SSL_VERIFY` — проверка SSL сертификатов: true/false (по умолчанию: false)
- `CONFLUENCE_URL` — базовый URL Confluence без завершающего `/` (on-prem: `https://confluence.corp.local`, при установке в контексте — `https://host/confluence`)
- `CONFLUENCE_USER` — логин для Basic auth (часто учётная запись с правами на страницы)
- `CONFLUENCE_TOKEN` — Personal Access Token, API token или пароль (в зависимости от настроек DC)
- `CONFLUENCE_API_TYPE` — по умолчанию **on-premises**: `server`, `onprem`, `datacenter` или `dc` — REST API v1 (`/rest/api`). Для **Atlassian Cloud** задайте `cloud` или `v2` (REST API v2, `/wiki/api/v2`)
- `CONFLUENCE_TIMEOUT` — таймаут в секундах (по умолчанию: 30)
- `CONFLUENCE_SSL_VERIFY` — проверка SSL: true/false (по умолчанию: false)

## Настройка

Скопируйте `.env_example` в `.env` и задайте нужные значения:

```bash
cp .env_example .env
# Отредактируйте .env (API ключи, Confluence и т.д.)
```

## Сборка

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

## Запуск

```bash
./build/poco_template_server
```

## Docker

```bash
# Создайте .env из .env_example перед запуском
cp .env_example .env

docker-compose up -d
# или
docker build -t poco_template_server .
docker run -p 8080:8080 --env-file .env poco_template_server
```

## Прогон тестов

```bash
docker build -f Dockerfile.test -t poco_ai_server_test . && docker run --rm poco_ai_server_test
```

## Формат ответа парсеров диаграмм

### PlantUML Sequence
- `components` — массив `{ id, code, name }`
- `requests` — массив `{ request_id, component_source_id, component_target_id, description }`

### PlantUML C4
- `components` — массив `{ id, code, name, c4_type }` (Person, SoftwareSystem, Container, Component)
- `requests` — массив `{ request_id, component_source_id, component_target_id, description }`
- `parent_child` — иерархия родитель-потомок `{ hierarchy_id, parent_id, child_id }` (Container внутри System, Component внутри Container)

### DrawIO C4
- `components` — массив `{ id, code, name, c4_type }` (Person, SoftwareSystem, Container, Component, ContainerDiagramTitle и др.)
- `requests` — массив `{ request_id, component_source_id, component_target_id, description }`
- `parent_child` — иерархия `{ hierarchy_id, parent_id, child_id }` (по геометрии вложенности)

### Parse Confluence
- `count` — количество найденных диаграмм
- `diagrams` — массив `{ text, format, subtype, sectionTitle }` (format: plantuml|drawio)

## Call examples

```bash
# AI processing
curl -X POST http://localhost:8080/api/v1/process_with_ai \
  -H "Content-Type: application/json" \
  -d '{"text": "Write C++ Hello World program"}'

# Load Confluence page (через наш API)
curl "http://localhost:8080/api/v1/load_confluence?page_id=1234567890&include_subpages=1"


# Загрузка и разбор страницы с подстраницами
curl "http://localhost:8080/api/v1/parse_confluence?page_id=1234567890&include_subpages=1"


# Только без подстраниц
curl "http://localhost:8080/api/v1/parse_confluence?page_id=1234567890"

# Разбор PlantUML Sequence диаграммы
curl -X POST http://localhost:8080/api/v1/parse_plantuml_sequence \
  -H "Content-Type: application/json" \
  -d '{"text":"@startuml\nparticipant User\nparticipant Frontend\nparticipant Backend\nUser -> Frontend: Login request\nFrontend -> Backend: Authenticate\nBackend --> Frontend: Auth token\n@enduml"}'

# Разбор PlantUML C4 диаграммы (формат rectangle)
curl -X POST http://localhost:8080/api/v1/parse_plantuml_c4 \
  -H "Content-Type: application/json" \
  -d '{"text":"@startuml\nrectangle \"==GlassFish\\n<size:10>[Software System]</size>\" <<GlassFish>> as GlassFish\nrectangle \"==DSTK\\n<size:10>[Software System]</size>\" <<DSTK>> as DSTK\nGlassFish .[#707070,thickness=2].> DSTK : \"deactivate\"\n@enduml"}'

# Разбор PlantUML C4 (C4-PlantUML: Person, System, Rel)
curl -X POST http://localhost:8080/api/v1/parse_plantuml_c4 \
  -H "Content-Type: application/json" \
  -d '{"text":"@startuml\n!include https://raw.githubusercontent.com/plantuml-stdlib/C4-PlantUML/master/C4_Context.puml\n\nPerson(customer, \"Покупатель\", \"Покупает товары в интернет-магазине\")\nPerson(admin, \"Администратор\", \"Управляет товарами и заказами в системе\")\nSystem(system, \"Интернет-магазин\", \"Система для покупки товаров онлайн\")\nSystem_Ext(paymentSystem, \"Платежная система\", \"Обрабатывает платежи\")\nSystem_Ext(shippingSystem, \"Система доставки\", \"Организует доставку товаров\")\nSystem_Ext(externalAuth, \"Система авторизации\", \"Проводит авторизацию пользователей через OAuth2\")\n\nRel(customer, system, \"Покупает товары\")\nRel(admin, system, \"Управляет товарами и заказами\")\nRel(system, paymentSystem, \"Запрос на оплату\")\nRel(system, shippingSystem, \"Запрос на доставку\")\nRel(system, externalAuth, \"Запрос на авторизацию через OAuth2\")\n@enduml"}'

# Разбор DrawIO C4. JSON с полем text, xml, content или drawio (XML диаграммы).
# source/target связей должны быть в атрибутах внутреннего mxCell.
curl -X POST http://localhost:8080/api/v1/parse_drawio \
  -H "Content-Type: application/json" \
  -d '{"text": "<mxfile host=\"app.diagrams.net\"><diagram><mxGraphModel><root><mxCell id=\"0\"/><mxCell id=\"1\" parent=\"0\"/><object id=\"sys1\" c4Name=\"Интернет-магазин\" c4Type=\"SoftwareSystem\"><mxCell parent=\"1\"/><mxGeometry x=\"20\" y=\"40\" width=\"120\" height=\"60\" as=\"geometry\"/></object><object id=\"sys2\" c4Name=\"Платёжная система\" c4Type=\"SoftwareSystem\"><mxCell parent=\"1\"/><mxGeometry x=\"200\" y=\"40\" width=\"120\" height=\"60\" as=\"geometry\"/></object><object id=\"rel1\" c4Type=\"Relationship\" c4Description=\"Запрос на оплату\"><mxCell parent=\"1\" source=\"sys1\" target=\"sys2\"/></object></root></mxGraphModel></diagram></mxfile>"}'
```

## Конвертация json в диаграмму

```bash
# из stdin
python3 scripts/json2dot.py < input.json

# из файла
python3 scripts/json2dot.py input.json

# вывод в файл
python3 scripts/json2dot.py input.json -o output.dot
```

Пример:

```bash
python3 scripts/json2dot.py test/fixtures/drawio/complex.json | dot -Tpng -o diagram.png
```