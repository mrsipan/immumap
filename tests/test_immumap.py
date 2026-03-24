import pytest

import immumap._core as immumap


def test_basic_operations():
    # 1. Start with an empty map
    m1 = immumap.Map()
    assert len(m1) == 0

    # 2. Set values (creates new maps)
    m2 = m1.set("a", 1)
    m3 = m2.set("b", 2)

    assert len(m2) == 1
    assert len(m3) == 2

    # 3. Verify immutability
    with pytest.raises(KeyError):
        _ = m1["a"]

    assert m2["a"] == 1
    with pytest.raises(KeyError):
        _ = m2["b"]

    assert m3["a"] == 1
    assert m3["b"] == 2


def test_repr_and_contains():
    m = immumap.Map().set("pi", 3.14).set("e", 2.71)

    # Test __contains__ (the 'in' operator)
    assert "pi" in m
    assert "kappa" not in m

    # Test __repr__
    r = repr(m)
    assert "Map({" in r
    assert "'pi': 3.14" in r
    assert "'e': 2.71" in r


def test_iteration():
    data = {"x": 100, "y": 200, "z": 300}
    m = immumap.Map()
    for k, v in data.items():
        m = m.set(k, v)

    # Test that we can iterate and find all keys
    keys = list(m)
    assert len(keys) == 3
    for k in data:
        assert k in keys


def test_collisions():
    # Force a hash collision if possible, or just high volume
    # This tests the HAMT tree depth logic
    m = immumap.Map()
    for i in range(1000):
        m = m.set(f"key_{i}", i)

    assert len(m) == 1000
    assert m["key_500"] == 500


if __name__ == "__main__":
    # Run tests manually if pytest isn't installed
    try:
        test_basic_operations()
        test_repr_and_contains()
        test_iteration()
        test_collisions()
        print("✅ All tests passed!")
    except Exception as e:
        print(f"❌ Test failed: {e}")
        raise
