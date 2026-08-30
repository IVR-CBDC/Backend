import pytest 
from calculator import load_corridors, find_corridor, calculate_commission


df = load_corridors()

def test_find_corridor_exists():
    corridor = find_corridor(df, "RU", "CN","CNY")
    assert corridor is not None
    assert corridor['corridor_id'] == 'RU-CN-CNY'


def test_find_corridor_not_exists():
    corridor = find_corridor(df,'RU','US','USD')
    assert corridor is None


def test_calculate_commission_formula():
    corridor = find_corridor(df,"RU", "CN","CNY") 
    result = calculate_commission(corridor,100000)
    assert result ['base_fee'] == 50.0
    assert result ['percentage_amount'] == 1500.0
    assert result ['fixed_fee'] == 25.0
    assert result ['total_commission'] == 1575.0


def test_calculate_min_fee():
    corridor = find_corridor(df,'RU','CN','CNY')
    result = calculate_commission(corridor,1000)
    assert result['total_commission'] >= result['min_fee']


def test_calculate_max_fee():
    corridor = find_corridor(df,'RU','CN', 'CNY')
    result = calculate_commission(corridor,999999)
    assert result['total_commission'] <= result ['max_fee']


def test_calculate_returns_all_fields():
    corridor = find_corridor(df,'RU','CN','CNY')
    result = calculate_commission(corridor,100000)
    assert 'corridor_id' in result
    assert 'base_fee' in result
    assert 'percentage_fee' in result
    assert 'percentage_amount' in result
    assert 'fixed_fee' in result
    assert 'total_commission' in result
    assert 'min_fee' in result
    assert 'max_fee' in result
    


