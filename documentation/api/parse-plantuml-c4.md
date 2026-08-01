# POST /api/v1/parse_plantuml_c4

Разбирает PlantUML C4-диаграмму. Поддерживает два стиля записи:

1. **Rectangle-стиль** (используется в C4-шаблонах с `rectangle "==Name" <<Type>> as alias`).
2. **C4-PlantUML библиотека** (`!include C4_Context.puml`; функции `Person(...)`, `System(...)`, `Container(...)`, `Component(...)`, `Rel(...)`).

## Запрос

- **Метод:** `POST`
- **Content-Type:** `application/json` (или raw-текст PlantUML)

Поля JSON (одно из — обязательно): `text`, `plantuml`, `content`.

```json
{
  "text": "@startuml\nrectangle \"==My System\" <<SoftwareSystem>> as sys1\nrectangle \"==External API\" <<Container>> as ext1\nsys1 .[#red].> ext1 : \"REST API\"\n@enduml"
}
```

## Успешный ответ — `200`

```json
{
  "success": true,
  "components": [
    { "id": "sys1", "code": "sys1", "name": "My System", "c4_type": "SoftwareSystem" },
    { "id": "ext1", "code": "ext1", "name": "External API", "c4_type": "Container" }
  ],
  "requests": [
    { "request_id": 1, "component_source_id": "sys1", "component_target_id": "ext1", "description": "REST API" }
  ],
  "parent_child": []
}
```

- `components[].c4_type` — один из: `Person`, `SoftwareSystem`, `Container`, `Component`, `SystemBoundary`, `ContainerBoundary` (зависит от метки/функции).
- `requests` — зависимости `source .[#color].> target : "message"` и `Rel(source, target, "message")` в порядке появления.
- `parent_child` — иерархия, когда контейнер/компонент вложен в родителя (C4-библиотека):

```json
{ "hierarchy_id": 1, "parent_id": "webapp", "child_id": "api" }
```

## Ошибки

| Статус | Условие |
|---|---|
| `400` | Не C4-диаграмма (нет `rectangle` + `.[].>`/C4-индикаторов, или нет `Person/System` + `Rel`) |
| `400` | Пустое тело, нет нужного поля, невалидный JSON |
| `405` | метод ≠ POST |

```json
{ "error": "Not a C4 diagram. Expected rectangle systems and .[].> dependencies.", "success": false }
```

## Определение типа диаграммы

Диаграмма считается C4, если выполнено одно из условий:
- есть `rectangle` и стрелки `.[` … `].>` (хотя бы по одному вхождению каждого);
- есть `rectangle` и C4-индикаторы (`software system`, `container`, `person`, `system boundary`);
- есть `Person(...)`/`System(...)` и `Rel(...)`.

## Примеры

**C4-библиотека:**

```plantuml
@startuml
!include https://raw.githubusercontent.com/plantuml-stdlib/C4-PlantUML/master/C4_Context.puml
Person(user, "Customer")
System(webapp, "Web Application")
Rel(user, webapp, "Browses")
@enduml
```

Результат: компоненты `user` (Person), `webapp` (SoftwareSystem), запрос `user → webapp : Browses`.

**Raw-тело:**

```bash
curl -X POST localhost:8080/api/v1/parse_plantuml_c4 \
  -H 'Content-Type: text/plain' \
  --data-binary '@c4.puml'
```

Подробности алгоритма — [algorithms/plantuml-c4.md](../algorithms/plantuml-c4.md).
