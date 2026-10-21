import sys
sys.path.insert(0, sys.prefix + "/lib/python3.11/site-packages")
import numpy as np

assert np.__version__ == "1.26.4"
a = np.arange(24, dtype=np.int32).reshape(4, 6)
assert a.dtype == np.dtype("int32")
assert a[1, 2] == 8 and a[::-1, ::2].tolist() == [[18, 20, 22], [12, 14, 16], [6, 8, 10], [0, 2, 4]]
broadcast = a + np.arange(6)
assert broadcast.sum() == 336
x = np.array([0., 0.5, 1.])
np.testing.assert_allclose(np.sin(x), [0., 0.479425538604203, 0.841470984807897])
np.testing.assert_allclose(np.exp(x), [1., 1.648721270700128, 2.718281828459045])
np.testing.assert_allclose(np.sqrt(np.array([1, 4, 9], dtype=np.float32)), [1, 2, 3])
np.testing.assert_allclose(np.sqrt(np.array([-4-0j, 3+4j])), [2j, 2+1j])
try:
    with np.errstate(divide="raise"):
        np.array([1.]) / np.array([0.])
except FloatingPointError:
    pass
else:
    raise AssertionError("numeric divide error was lost")
for kind, value, operation in (("invalid", -1., np.sqrt), ("over", 1000., np.exp)):
    try:
        with np.errstate(**{kind: "raise"}):
            operation(np.array([value]))
    except FloatingPointError:
        pass
    else:
        raise AssertionError("numeric " + kind + " error was lost")

for dtype in (np.float32, np.float64, np.complex64, np.complex128):
    a = np.array([[1, 2, 3], [4, 5, 6]], dtype=dtype)
    b = np.array([[7, 8], [9, 10], [11, 12]], dtype=dtype)
    np.testing.assert_allclose(a @ b, [[58, 64], [139, 154]], rtol=2e-5)
    np.testing.assert_allclose(a @ np.array([2, 3, 4], dtype=dtype), [20, 47], rtol=2e-5)
    np.testing.assert_allclose(a @ a.T, [[14, 32], [32, 77]], rtol=2e-5)
    padded = np.zeros((4, 6), dtype=dtype)
    padded[::2, ::2] = a
    np.testing.assert_allclose(padded[::2, ::2] @ b, [[58, 64], [139, 154]], rtol=2e-5)
    matrix = np.array([[0, 1], [2, 3]], dtype=dtype)
    rhs = np.array([1, 5], dtype=dtype)
    np.testing.assert_allclose(np.linalg.solve(matrix, rhs), [1, 1], rtol=2e-5)
    np.testing.assert_allclose(matrix @ np.linalg.inv(matrix), np.eye(2), atol=2e-5)
    np.testing.assert_allclose(np.linalg.det(matrix), -2, rtol=2e-5)
    positive = np.array([[4, 2], [2, 5]], dtype=dtype)
    lower = np.linalg.cholesky(positive)
    np.testing.assert_allclose(lower @ lower.conj().T, positive, rtol=2e-5)
    design = np.array([[1, 0], [0, 1], [1, 1]], dtype=dtype)
    fitted, residuals, rank, singular = np.linalg.lstsq(design, np.array([1, 2, 4], dtype=dtype), rcond=None)
    np.testing.assert_allclose(fitted, [4/3, 7/3], rtol=2e-5)
    np.testing.assert_allclose(residuals, [1/3], rtol=2e-5)
    np.testing.assert_allclose(singular, [np.sqrt(3), 1], rtol=2e-5)
    assert rank == 2
    try:
        np.linalg.solve(np.array([[1, 2], [2, 4]], dtype=dtype), rhs)
    except np.linalg.LinAlgError:
        pass
    else:
        raise AssertionError("singular LU solve was accepted")

z = np.array([1+2j, 3-1j])
w = np.array([2-1j, 4+3j])
np.testing.assert_allclose(np.vdot(z, w), 9+8j)
np.testing.assert_allclose(np.dot(z, w), 19+8j)
print("Alchemy NumPy: arrays BLAS LU Cholesky")
