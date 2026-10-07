# invalid arguments to micropython.native and micropython.viper


def test_syntax(code):
    try:
        exec(code)
    except SyntaxError:
        print("SyntaxError")


test_syntax("@micropython.native(optional=1)\ndef f(): pass")
test_syntax("@micropython.viper(True)\ndef f(): pass")
test_syntax("@micropython.viper(other=True)\ndef f(): pass")
test_syntax("@micropython.native(optional=True, x=1)\ndef f(): pass")
