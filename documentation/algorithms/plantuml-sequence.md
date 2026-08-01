# Алгоритм: PlantUML Sequence

Реализация: `src/plantuml/plantuml_sequence.h` → `PlantUmlSequenceParser::parse`.

## Общая схема

```
plantumlText
   │
   ├─ 1. isSequenceDiagram()?  ── нет → {error, success:false}
   │
   ├─ 2. extractParticipants()  — participant/actor, alias → name
   │
   ├─ 3. extractInteractions()  — стрелки с сообщениями, сортировка по позиции
   │
   ├─ 4. если участников нет, но есть взаимодействия:
   │        extractParticipantsFromArrows() — восстановить участников из стрелок
   │
   └─ 5. buildJson() → {success, components, requests}
```

## Шаг 1. Определение типа диаграммы

Диаграмма считается Sequence, если в тексте (в нижнем регистре) есть:
- `participant` **или** `actor`, **или**
- стрелка `->` **или** `-->`.

Если ни того, ни другого нет — возвращается ошибка:
```
"Not a sequence diagram. Only participant/actor and -> interactions are supported."
```

## Шаг 2. Извлечение участников

Регулярное выражение (регистронезависимое):

```
(?:participant|actor)\s+(?:"([^"]+)"|([^\s]+))(?:\s+as\s+([^\s]+))?
```

- Группа 1 — имя в кавычках (`participant "Auth Service" as Auth` → name=`Auth Service`).
- Группа 2 — имя без кавычек.
- Группа 3 — псевдоним после `as`; если его нет, псевдоним = имя.

В `components` попадает: `id`/`code` = псевдоним, `name` = отображаемое имя.

## Шаг 3. Извлечение взаимодействий

Паттерны стрелок с сообщением (в порядке приоритета):

| # | Паттерн |
|---|---|
| 1 | `source -->> target : message` |
| 2 | `source --> target : message` |
| 3 | `source ->> target : message` |
| 4 | `source -> target : message` |

Для каждого паттерна находятся все совпадения. Далее:

1. Все совпадения собираются и **сортируются по позиции** в тексте (`start`).
2. Удаляются **перекрывающиеся** совпадения (остаётся первое встреченное — это исключает повторный «прогон» одного и того же фрагмента разными паттернами).
3. Каждому оставшемуся присваивается `request_id` (шаг) в порядке возрастания позиции.
4. `source`/`target` очищаются (`cleanParticipant`), из `message` вырезаются HTML-теги (`stripHtml`).

### Очистка имён (`cleanParticipant`)

- Убираются хвостовые разделители `:` и пробелы.
- Убирается нумерация строк вида `1.2. `, `2. `.
- Убирается суффикс-статус: `: OK`, `: ERROR`, `: 200`, `: 400 (…)` и т.п.
- Пустые/невалидные участники отбрасываются (`isValidParticipant`): длина ≤ 1, содержит `-`/`>`/`<`/`>>`, заканчивается `.`, является числом, является аббревиатурой из 2–4 заглавных букв.

## Шаг 4. Восстановление участников из стрелок

Если явных `participant`/`actor` не объявлено, но есть взаимодействия, участники восстанавливаются:

- По паттернам `->`, `-->`, `->>`, `-->>`, `<<-`, `<--` собираются все «полюса».
- Каждый полюс очищается и проходит `isValidParticipant`.
- Отсортированное множество попадает в `components` (id = code = name).

## Шаг 5. Формирование JSON

```json
{
  "success": true,
  "components": [ { "id": "User", "code": "User", "name": "User" }, … ],
  "requests": [
    { "request_id": 1, "component_source_id": "User", "component_target_id": "API", "description": "Login request" },
    …
  ]
}
```

Поле `parent_child` для Sequence **не формируется**.

## Особенности и ограничения

- Порядок `requests` соответствует порядку строк диаграммы (сортировка по позиции), что важно для sequence-аналитики.
- Вложенные блоки (`loop`, `alt`, `opt`, `group`) не распознаются как конструкции — их содержимое просто попадает в поток стрелок.
- Async-стрелки (`-/>`, `-\`) не поддерживаются.
- `note`, `activate`/`deactivate` игнорируются (кроме влияния на определение типа).

## Тесты

- `test/fixtures/plantuml_sequence/simple_sequence.puml` — классический пример с 4 сообщениями.
- `test/fixtures/plantuml_sequence/arrows_only.puml` — без явных participant (восстановление из стрелок).
- `test/fixtures/plantuml_sequence/large_sequence.puml` — большая диаграмма (проверка производительности и отсутствия переполнения стека).
