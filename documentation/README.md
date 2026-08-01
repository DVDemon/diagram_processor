# Документация проекта "Diagram Processing Server" (poco_ai_server)

REST-сервер на C++ (POCO) для разбора архитектурных диаграмм (PlantUML Sequence/C4, DrawIO C4), интеграции с Confluence и AI (DeepSeek/OpenAI).

## Содержание

### Общее
| Раздел | Описание |
|---|---|
| [Архитектура](architecture.md) | Структура кода, поток запросов, компоненты, схема обработки |
| [Конфигурация](configuration.md) | Все переменные окружения, таблица параметров |
| [Сборка и запуск](build-and-run.md) | Сборка, тесты, Docker, docker-compose, CI/CD |

### API (документация эндпоинтов)
| Раздел | Описание |
|---|---|
| [API — общие соглашения](api/overview.md) | Формат запросов/ответов, коды ошибок, логирование, метрики |
| [POST /api/v1/process_with_ai](api/process-with-ai.md) | Отправка текста в AI (DeepSeek/OpenAI) |
| [POST /api/v1/parse_plantuml_sequence](api/parse-plantuml-sequence.md) | Разбор PlantUML Sequence |
| [POST /api/v1/parse_plantuml_c4](api/parse-plantuml-c4.md) | Разбор PlantUML C4 |
| [POST /api/v1/parse_drawio](api/parse-drawio.md) | Разбор DrawIO C4 |
| [GET /api/v1/load_confluence](api/load-confluence.md) | Загрузка страницы Confluence |
| [GET /api/v1/parse_confluence](api/parse-confluence.md) | Загрузка и извлечение диаграмм из страницы Confluence |
| [GET /metrics](api/metrics.md) | Метрики Prometheus |
| [GET /swagger.yaml](api/swagger.md) | OpenAPI-спецификация |

### Алгоритмы парсеров
| Раздел | Описание |
|---|---|
| [PlantUML Sequence](algorithms/plantuml-sequence.md) | Алгоритм извлечения участников и взаимодействий |
| [PlantUML C4](algorithms/plantuml-c4.md) | Алгоритм извлечения компонентов C4 и зависимостей |
| [DrawIO](algorithms/drawio.md) | Алгоритм разбора XML, геометрия, восстановление «сломанных» связей |
| [JSON → DOT](algorithms/json2dot.md) | Конвертация результата в Graphviz |

### Интеграция с Confluence
| Раздел | Описание |
|---|---|
| [Интеграция с Confluence](confluence/overview.md) | Обзор, авторизация, REST API v1 (on-prem) vs v2 (Cloud), контекстный путь |
| [Загрузка страниц](confluence/page-loading.md) | Загрузка по ID/URL, include-макросы, дерево подстраниц, защита от циклов |
| [Анализ вложений](confluence/attachments.md) | Поиск и извлечение DrawIO-вложений по содержимому |
| [Извлечение диаграмм](confluence/diagram-extraction.md) | PlantUML/DrawIO из HTML: макросы, inline-код, дедупликация |

### Интеграция с AI
| Раздел | Описание |
|---|---|
| [AI / OpenAI / DeepSeek](ai.md) | Клиент, формат запросов, обработка ошибок |

---

## Краткая справка по эндпоинтам

```
POST /api/v1/process_with_ai          — обработка текста через AI
POST /api/v1/parse_plantuml_sequence  — разбор PlantUML Sequence → {components, requests}
POST /api/v1/parse_plantuml_c4        — разбор PlantUML C4 → {components, requests, parent_child}
POST /api/v1/parse_drawio             — разбор DrawIO C4 → {components, requests, parent_child}
GET  /api/v1/load_confluence?page_id={id}[&include_subpages=1] — HTML страницы (+дерево)
GET  /api/v1/parse_confluence?page_id={id}[&include_subpages=1] — диаграммы из страницы
GET  /metrics                        — метрики Prometheus
GET  /swagger.yaml                   — OpenAPI 3.0 спецификация
```

Быстрый старт:

```bash
cp .env_example .env        # задать ключи/URL
docker compose up --build   # или локальная сборка (см. build-and-run.md)
curl -X POST localhost:8080/api/v1/parse_drawio \
  -H 'Content-Type: application/json' \
  -d '{"text": "<mxfile...>..."}'
```
