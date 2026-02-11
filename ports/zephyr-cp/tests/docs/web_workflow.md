# Web Workflow native_sim Tests

These tests validate CircuitPython's web workflow support in the Zephyr native_sim port, including filesystem write behavior with and without USB-style write protection.

## Coverage

- `test_web_workflow_hostnetwork`: Verifies the web workflow HTTP server responds and enforces authentication (`/edit/` returns `401 Unauthorized`).
- `test_web_workflow_write_code_py_conflict`: Exercises a write attempt while the filesystem is protected (no `boot.py` remount). The DELETE request should return `409 Conflict`.
- `test_web_workflow_write_code_py_remount`: Uses a `boot.py` remount to allow CircuitPython to write. A PUT request updates `code.py`, and a subsequent GET verifies the contents.

## Filesystem Setup

The tests create a flash image with:

- `settings.toml` containing `CIRCUITPY_WEB_API_PASSWORD="testpass"` so the web workflow starts using the on-device settings file.
- `boot.py` (for the remount test only) with:
  ```python
  import storage
  storage.remount("/", readonly=False)
  ```
  This disables concurrent write protection so the web workflow can write to CIRCUITPY.

## Running the Tests

Build native_sim (if needed):

```bash
make BOARD=native_native_sim
```

Run the tests:

```bash
pytest -q ports/zephyr-cp/tests/test_web_workflow.py::test_web_workflow_hostnetwork -vv
pytest -q ports/zephyr-cp/tests/test_web_workflow.py::test_web_workflow_write_code_py_conflict -vv
pytest -q ports/zephyr-cp/tests/test_web_workflow.py::test_web_workflow_write_code_py_remount -vv
```
