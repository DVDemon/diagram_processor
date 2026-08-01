# POST /api/v1/parse_drawio

Разбирает DrawIO (diagrams.net) C4-диаграмму. Поддерживает **оба формата хранения диаграммы в `.drawio`**:
- **несжатый XML** (`<mxfile><diagram><mxGraphModel>…`),
- **сжатый формат** (base64 + deflate/zlib-инфляция + percent-decoding внутри `<diagram>`).

## Запрос

- **Метод:** `POST`
- **Content-Type:** `application/json` (или raw-XML)

Поля JSON (одно из — обязательно): `text`, `xml`, `content`, `drawio`.

```json
{
  "text": "<mxfile host=\"app.diagrams.net\"><diagram><mxGraphModel><root><mxCell id=\"0\"/><mxCell id=\"1\" parent=\"0\"/><object id=\"sys1\" c4Name=\"Интернет-магазин\" c4Type=\"SoftwareSystem\"><mxCell parent=\"1\"/><mxGeometry x=\"20\" y=\"40\" width=\"120\" height=\"60\" as=\"geometry\"/></object><object id=\"sys2\" c4Name=\"Платёжная система\" c4Type=\"SoftwareSystem\"><mxCell parent=\"1\"/><mxGeometry x=\"200\" y=\"40\" width=\"120\" height=\"60\" as=\"geometry\"/></object><object id=\"rel1\" c4Type=\"Relationship\" c4Description=\"Запрос на оплату\"><mxCell parent=\"1\" source=\"sys1\" target=\"sys2\"/></object></root></mxGraphModel></diagram></mxfile>"
}
```

## Успешный ответ — `200`

```json
{
  "success": true,
  "components": [
    { "id": "sys1", "code": "sys1", "name": "Интернет-магазин", "c4_type": "SoftwareSystem" },
    { "id": "sys2", "code": "sys2", "name": "Платёжная система", "c4_type": "SoftwareSystem" }
  ],
  "requests": [
    { "request_id": 1, "component_source_id": "sys1", "component_target_id": "sys2", "description": "Запрос на оплату" }
  ],
  "parent_child": []
}
```

## Особенности

- **Компоненты** — элементы `<object>` с атрибутом `c4Type` (не `SystemScopeBoundary`/`ContainerScopeBoundary`). Имя берётся из `c4Name`, затем `c4Description`, затем `c4Type`.
- **Связи** — `<object c4Type="Relationship">`, где `source`/`target` лежат во **внутреннем `<mxCell>`** (не в `<object>`).
- **Иерархия** `parent_child` строится по **геометрии** (вложенность прямоугольников). Границы (`SystemScopeBoundary`, `ContainerScopeBoundary`) исключаются из результата.
- **Восстановление связей**: если у `Relationship` нет `source`/`target`, но есть `mxPoint` (sourcePoint/targetPoint), парсер по точке находит ближайший (наименьший по площади) компонент, в который попадает точка.
- **Связи из `mxCell` со стилем `edgeStyle=`** без атрибутов `source`/`target` собираются в `broken` и пытаются восстановиться по точкам.
- **Дедупликация**: дублирующиеся пары `(source, target)` отбрасываются; связи с неизвестными компонентами игнорируются.

## Ошибки

| Статус | Условие |
|---|---|
| `400` | Нет `<mxfile>`/`<mxGraphModel>`, не удалось извлечь содержимое диаграммы, пустое тело, нет поля |
| `400` | Невалидный JSON во входе |
| `405` | метод ≠ POST |

```json
{ "error": "Not a DrawIO diagram. Expected mxfile or mxGraphModel XML.", "success": false }
```

## Пример через raw-XML

```bash
curl -X POST localhost:8080/api/v1/parse_drawio \
  -H 'Content-Type: text/plain' \
  --data-binary '@diagram.drawio'
```

Подробности алгоритма — [algorithms/drawio.md](../algorithms/drawio.md).
