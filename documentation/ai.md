# Интеграция с AI (OpenAI / DeepSeek)

Реализация: `src/openai/openai_client.h` → `openai::OpenAIClient`. Используется эндпоинтом `POST /api/v1/process_with_ai`.

## Принцип работы

`OpenAIClient` — минималистичный клиент к OpenAI-совместимому **Chat Completions API**. Может работать с любым провайдером, реализующим этот формат: DeepSeek, OpenAI, OpenAI-совместимые шлюзы (vLLM, Ollama через прокси и т.п.).

## Формирование запроса

### URL

```
{OPENAI_API_URL}/v1/chat/completions        # например https://api.deepseek.com/v1/chat/completions
```

Правило дописывания пути:
- если базовый URL не содержит пути (или он пуст/`/`) → добавляется `/v1/chat/completions`;
- если базовый URL уже содержит путь, оканчивающийся на `/` → добавляется `chat/completions`;
- иначе → `/chat/completions`.

### Тело (JSON)

```json
{
  "model": "deepseek-chat",
  "messages": [
    { "role": "system", "content": "…OPENAI_SYSTEM_PROMPT…" },
    { "role": "user", "content": "…текст пользователя…" }
  ]
}
```

- `system`-сообщение включается только если задан `OPENAI_SYSTEM_PROMPT` (или передан override).
- Эндпоинт `process_with_ai` передаёт `systemPromptOverride` = `OPENAI_SYSTEM_PROMPT` из окружения.

### Заголовки

```
Content-Type: application/json
Authorization: Bearer {OPENAI_API_KEY}
```

### Транспорт

- `HTTPSClientSession(host, port, sslContext)`.
- SSL-контекст: `VERIFY_STRICT` при `OPENAI_SSL_VERIFY=true`, иначе `VERIFY_NONE`.
- Таймаут сессии: `OPENAI_TIMEOUT` секунд.

## Разбор ответа

Ожидается формат:

```json
{
  "choices": [
    { "message": { "content": "…ответ модели…" } }
  ]
}
```

Извлекается `choices[0].message.content`. Ошибки разбора → `std::runtime_error("Failed to parse OpenAI response: …")`.

## Ошибки и коды

| Условие | Исключение | HTTP-статус эндпоинта |
|---|---|---|
| `OPENAI_API_KEY` пуст | `runtime_error("OPENAI_API_KEY is not set")` | `503` |
| Статус API ≠ 200 | `runtime_error("OpenAI API error: {status} - {body}")` | `502` |
| Нет `choices`/`message` | `runtime_error("No choices/No message…")` | `502` |
| Некорректный JSON во входе | `Poco::Exception` | `400` |

## Расширяемость

- Чтобы использовать OpenAI напрямую: `OPENAI_API_URL=https://api.openai.com`, `OPENAI_MODEL=gpt-4o-mini` (или новейшая модель по вашему выбору).
- Локальный сервер: `OPENAI_API_URL=http://localhost:11434/v1` (Ollama OpenAI-совместимый эндпоинт), `OPENAI_MODEL=llama3`.
- System-промпт в `.env_example` задаёт поведение «точность важнее уместности», но его можно переопределить под задачу (например, «Ты — архитектор. Анализируй диаграммы…»).

## Ограничения

- Только режим `chat/completions` (без стриминга, без инструментов, без vision).
- Без поддержки повторных попыток/ретраев при сетевых сбоях.
- Держит соединение на один запрос (новая сессия на каждый вызов).
