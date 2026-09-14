# Commission Calculation Service

Микросервис для расчёта комиссии трансграничных платежей 

## Что делает сервис

- Принимает параметры перевода (страна отправителя, страна получателя, валюта, сумма)
- Сам определяет валютный коридор
- Рассчитывает комиссию по данной формуле: `base_fee + (amount × percentage_fee) + fixed_fee`
- Возвращает детальный breakdown комиссии

## Запускается сервис через Docker Compose

```bash
docker compose up

```
## После запуска откройте в браузере http://localhost:8000/docs

## Пример запроса и ответа: 

## Запрос:
{
  "from_country": "RU",
  "to_country": "CN",
  "currency": "CNY",
  "amount": 100000
}

## Ответ:
{
  "corridor_id": "RU-CN-CNY",
  "base_fee": 50.0,
  "percentage_fee": 0.015,
  "percentage_amount": 1500.0,
  "fixed_fee": 25.0,
  "total_commission": 1575.0
}

