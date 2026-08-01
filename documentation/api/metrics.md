# GET /metrics

Отдаёт метрики в формате Prometheus (text exposition format).

## Запрос

```
GET /metrics
```

## Метрики

| Метрика | Тип | Описание |
|---|---|---|
| `http_requests_total` | Counter | Общее число HTTP-запросов |
| `http_errors_total` | Counter | Число ответов с ошибкой (4xx, 5xx) |
| `http_request_duration_seconds` | Histogram | Гистограмма длительности запросов в секундах |

Бакеты гистограммы `http_request_duration_seconds`:

```
0.0001, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0
```

Каждый обработчик (включая `NotFoundHandler` и `MetricsHandler`) инкрементирует счётчики и пишет длительность в гистограмму. Метрики регистрируются в `Poco::Prometheus::Registry::defaultRegistry()` и отдаются через `Poco::Prometheus::MetricsRequestHandler`.

## Пример ответа

```
# HELP http_requests_total Total number of HTTP requests
# TYPE http_requests_total counter
http_requests_total 42
# HELP http_errors_total Total number of HTTP error responses (4xx, 5xx)
# TYPE http_errors_total counter
http_errors_total 3
# HELP http_request_duration_seconds HTTP request duration in seconds
# TYPE http_request_duration_seconds histogram
http_request_duration_seconds_bucket{le="0.001"} 5
…
http_request_duration_seconds_sum 1.234
http_request_duration_seconds_count 42
```

## Интеграция с Prometheus/Grafana

```yaml
# prometheus.yml (пример)
scrape_configs:
  - job_name: poco_server
    static_configs:
      - targets: ['host:8080']
```

Метрики глобальные (процессные) — не зависят от маршрута. Для разбивки по эндпоинтам нужен экспорт из логов либо доработка обработчиков.
