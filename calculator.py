import pandas as pd

def load_corridors(path: str = "data/corridors.csv") -> pd.DataFrame:
    return pd.read_csv(path)

def find_corridor(df: pd.DataFrame, from_country: str, to_country: str, currency: str):
    result = df[
        (df["from_country"] == from_country) &
        (df["to_country"] == to_country) &
        (df["currency"] == currency)
    ]
    if result.empty:
        return None
    return result.iloc[0]

def calculate_commission(corridor, amount: float) -> dict:
    base_fee = float(corridor["base_fee"])
    percentage_fee = float(corridor["percentage_fee"])
    fixed_fee = float(corridor["fixed_fee"])
    min_fee = float(corridor["min_fee"])
    max_fee = float(corridor["max_fee"])

    percentage_amount = amount * percentage_fee
    total = base_fee + percentage_amount + fixed_fee

    if total < min_fee:
        total = min_fee
    if total > max_fee:
        total = max_fee

    return {
        "corridor_id": corridor["corridor_id"],
        "from_country": corridor["from_country"],
        "to_country": corridor["to_country"],
        "currency": corridor["currency"],
        "amount": amount,
        "base_fee": base_fee,
        "percentage_fee": percentage_fee,
        "percentage_amount": round(percentage_amount, 2),
        "fixed_fee": fixed_fee,
        "total_commission": round(total, 2),
        "min_fee": min_fee,
        "max_fee": max_fee
    }
