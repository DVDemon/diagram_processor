# Асинхронный API для AI (OpenAI / DeepSeek)

Асинхронная версия `process_with_ai`: задача выполняется в **пуле потоков POCO** (`Poco::ThreadPool`), а клиент опрашивает статус и результат. Полезно для длинных промптов и чтобы не блокировать HTTP-воркер.

Реализация: `src/openai/async_openai_manager.h` + обработчики `process_with_ai_async`, `async_ai_status`, `async_ai_result`.

## Три эндпоинта

### 1. Запуск задачи — `POST /api/v1/process_with_ai_async`

- **Метод:** `POST`, `Content-Type: application/json`
- **Поля** (одно из — обязательно): `text`, `prompt`, `message`

```json
{ "text": "Опиши архитектуру этого сервиса" }
```

**Ответ — `202 Accepted`:**

```json
{ "request_id": 1785598922016154, "status": "running" }
```

`request_id` — int64, генерируется при старте задачи и хранится в памяти (map).

### 2. Статус — `GET /api/v1/async_ai_status?request_id={id}`

**Ответ — `200`:**

```json
{
  "request_id": 1785598922016154,
  "status": "running",          // running | completed | failed
  "start_time_ms": 1785598922016,  // время старта процесса (epoch, мс)
  "retries": 0,                    // количество перепосылов запросов к AI
  "bytes_sent": 0                  // суммарно отправлено байт в ИИ (за все попытки)
}
```

### 3. Результат — `GET /api/v1/async_ai_result?request_id={id}`

| Статус задачи | HTTP | Тело |
|---|---|---|
| `running` | `202` | `{ "request_id": …, "status": "running" }` |
| `completed` | `200` | `{ "request_id": …, "status": "completed", "result": "…ответ LLM…" }` |
| `failed` | `200` | `{ "request_id": …, "status": "failed", "error": "…описание ошибки…" }` |

## Ошибки

| Статус | Условие |
|---|---|
| `400` | нет поля `text/prompt/message`, пустое тело, не JSON (`process_with_ai_async`); нет/нечисловой `request_id` (`async_ai_status`/`async_ai_result`) |
| `404` | `request_id` не найден (задача удалена по лимиту или id неверный) |
| `503` | `OPENAI_API_KEY` не задан |
| `405` | неверный метод |

## Поведение и ретраи

- Задача выполняется асинхронно в `Poco::ThreadPool` (размер — `OPENAI_ASYNC_MAX_THREADS`).
- **Ретраи только на транзиентные ошибки**: при HTTP-статусе ответа `429` (rate limit) или `5xx` запрос **перепосылается** до `OPENAI_MAX_RETRIES` раз (по умолчанию 3), с нарастающей паузой. Остальные ошибки (`4xx`, отсутствие ключа, ошибка разбора, сеть) завершают задачу **сразу**, без перепосылок.
- Счётчик пересылок виден в `retries`, объём отправленных байт (за все попытки) — в `bytes_sent`.
- Задачи хранятся в памяти; при превышении `OPENAI_ASYNC_MAX_JOBS` удаляются самые старые завершённые.
- Задача исчезает после перезапуска сервера (память, не БД).

## Конфигурация

| Переменная | По умолчанию | Описание |
|---|---|---|
| `OPENAI_ASYNC_MAX_THREADS` | `4` | размер пула потоков |
| `OPENAI_MAX_RETRIES` | `3` | число повторных пересылок при `429`/`5xx` |
| `OPENAI_ASYNC_MAX_JOBS` | `10000` | максимум хранимых задач |

## Пример

```bash
# 1. Запустить задачу
RID=$(curl -s -X POST localhost:8080/api/v1/process_with_ai_async \
  -H 'Content-Type: application/json' \
  -d '{"text":"Напиши краткое резюме этой архитектуры"}' \
  | python3 -c "import sys,json; print(json.load(sys.stdin)['request_id'])")

# 2. Статус
curl "localhost:8080/api/v1/async_ai_status?request_id=$RID"

# 3. Результат (поллинг, пока status != running)
curl "localhost:8080/api/v1/async_ai_result?request_id=$RID"
```
