# POST /api/v1/process_with_ai

Отправляет текст пользователя в OpenAI-совместимый чат-API (по умолчанию DeepSeek) и возвращает ответ модели.

## Запрос

- **Метод:** `POST`
- **Content-Type:** `application/json`
- **Поля тела** (одно из — обязательно):
  - `text` — текст/промпт для AI;
  - `prompt` — альтернативное имя поля;
  - `message` — альтернативное имя поля.

```json
{
  "text": "Опиши архитектуру этого сервиса"
}
```

## Успешный ответ — `200`

```json
{
  "result": "…текст от модели…",
  "success": true
}
```

## Ответы с ошибками

| Статус | Условие | Тело |
|---|---|---|
| `400` | нет поля `text/prompt/message`, пустое поле, пустое тело, не JSON | `{"error": "Request must contain 'text', 'prompt' or 'message' field"}` и т.п. |
| `502` | ошибка API модели | `{"error": "OpenAI API error: {status} - {body}", "success": false}` |
| `503` | `OPENAI_API_KEY` не задан | `{"error": "AI service not configured. OPENAI_API_KEY is required."}` |
| `500` | внутренняя ошибка | `{"error": "Internal error", "success": false}` |
| `405` | метод ≠ POST | `{"error":"Method not allowed"}` |

## Конфигурация

Параметры клиента задаются переменными окружения (см. [configuration.md](../configuration.md)):

| Переменная | По умолчанию | Назначение |
|---|---|---|
| `OPENAI_API_KEY` | — (обязательна) | токен API |
| `OPENAI_API_URL` | `https://api.deepseek.com` | базовый URL |
| `OPENAI_MODEL` | `deepseek-chat` | имя модели |
| `OPENAI_SYSTEM_PROMPT` | — | системный промпт |
| `OPENAI_TIMEOUT` | `60` | таймаут, сек |
| `OPENAI_SSL_VERIFY` | `false` | проверка SSL-сертификатов |

## Поведение

1. Собирается запрос `POST {OPENAI_API_URL}/v1/chat/completions` (если базовый URL уже оканчивается на `/v1`, путь дописывается как `/chat/completions`).
2. Формируются `messages`: опциональный `system` (из `OPENAI_SYSTEM_PROMPT`) и обязательный `user`.
3. Отправляется через `HTTPSClientSession` (Basic/нет — используется `Authorization: Bearer`).
4. Из ответа извлекается `choices[0].message.content`.

### Пример

```bash
curl -X POST localhost:8080/api/v1/process_with_ai \
  -H 'Content-Type: application/json' \
  -d '{"text": "Напиши краткое резюме этой архитектуры"}'
```
