# Test that "UTF-8" is accepted as an encoding name, as in CPython.
# See https://github.com/adafruit/circuitpython/issues/11580

print(bytes("hi", "utf-8"))
print(bytes("hi", "UTF-8"))
print("hi".encode("UTF-8"))
print(b"hi".decode("UTF-8"))
try:
    bytes("hi", "latin-1")
except LookupError:
    print("LookupError")
