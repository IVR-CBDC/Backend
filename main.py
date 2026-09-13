from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
from calculator import load_corridors, find_corridor, calculate_commission

app = FastAPI(title="Commission Calculation Service")

corridors_df = load_corridors()
class PaymentRequest(BaseModel):
    from_country: str
    to_country: str
    currency: str
    amount: float

@app.post("/calculate")
def calculate(request: PaymentRequest):

    if request.from_country == request.to_country:
        raise HTTPException(status_code=400, detail="SAME_COUNTRY")

    if request.amount <= 0:
        raise HTTPException(status_code=400, detail="INVALID_AMOUNT")

    corridor = find_corridor(
        corridors_df,
        request.from_country.upper(),
        request.to_country.upper(),
        request.currency.upper()
    )

    if corridor is None:
        raise HTTPException(status_code=404, detail="CORRIDOR_NOT_FOUND")

    if request.amount < corridor["min_limit"]:
        raise HTTPException(status_code=400, detail="AMOUNT_BELOW_LIMIT")

    if request.amount > corridor["max_limit"]:
        raise HTTPException(status_code=400, detail="AMOUNT_ABOVE_LIMIT")

    return calculate_commission(corridor, request.amount)

@app.get("/health")
def health():
    return {"status": "ok"}

