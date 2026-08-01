# POST /api/v1/parse_plantuml_sequence

Разбирает PlantUML Sequence-диаграмму: извлекает участников (`participant`/`actor`) и их взаимодействия (стрелки с сообщениями).

## Запрос

- **Метод:** `POST`
- **Content-Type:** `application/json` (или raw-текст PlantUML в теле)

Поля JSON (одно из — обязательно): `text`, `plantuml`, `content`.

```json
{
  "text": "@startuml\nparticipant User\nparticipant API\nUser -> API : Login request\nAPI --> User : Auth token\n@enduml"
}
```

## Успешный ответ — `200`

```json
{
  "success": true,
  "components": [
    { "id": "User", "code": "User", "name": "User" },
    { "id": "API",  "code": "API",  "name": "API" }
  ],
  "requests": [
    { "request_id": 1, "component_source_id": "User", "component_target_id": "API", "description": "Login request" },
    { "request_id": 2, "component_source_id": "API",  "component_target_id": "User", "description": "Auth token" }
  ]
}
```

- `components` — участники: `id`/`code` = псевдоним (alias), `name` = отображаемое имя (из кавычек или псевдоним).
- `requests` — взаимодействия в порядке появления на диаграмме (`request_id` нумеруется с 1).
- `parent_child` — **отсутствует** (для Sequence не строится).

## Ошибки

| Статус | Условие |
|---|---|
| `400` | Не sequence-диаграмма (нет `participant`/`actor` и стрелок `->`/`-->`), пустое тело, нет нужного поля |
| `400` | Невалидный JSON во входе |
| `405` | метод ≠ POST |

Пример ошибки:

```json
{ "error": "Not a sequence diagram. Only participant/actor and -> interactions are supported.",
  "success": false }
```

## Поддерживаемый синтаксис

- `participant "Name" as alias` / `actor "Name" as alias`, включая `participant Name` (без кавычек).
- Стрелки: `->`, `-->`, `->>`, `-->>`, `<<-`, `<--` — взаимодействия извлекаются для первых четырёх видов с сообщением `: message`.
- Если участники не объявлены явно, они восстанавливаются из стрелок.
- HTML-теги в сообщениях вырезаются (`<b>text</b>` → `text`).
- Мусор из имён чистится: номера строк, служебные суффиксы (`OK`, `200`, `: ERROR` и т.п.).

### Пример через raw-тело

```bash
curl -X POST localhost:8080/api/v1/parse_plantuml_sequence \
  -H 'Content-Type: text/plain' \
  --data-binary '@diagram.puml'
```

Подробности алгоритма — [algorithms/plantuml-sequence.md](../algorithms/plantuml-sequence.md).
