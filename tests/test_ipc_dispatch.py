"""The overlay's reading of the core's `mode` event, without a pipe."""

from keygnosys.coreclient.ipc import IpcClient


def _mode(client, **data):
    client._dispatch({"v": 1, "t": "event", "n": "mode", "d": data})


def test_mode_event_drives_inert_changed_both_ways(qtbot):
    client = IpcClient("keygnosys-test-unused")
    seen = []
    client.inertChanged.connect(seen.append)

    _mode(client, mode="normal", latched=False, activation="hybrid", inert=True)
    _mode(client, mode="normal", latched=False, activation="hybrid", inert=False)

    assert seen == [True, False]


def test_mode_event_without_inert_reads_as_not_inert(qtbot):
    # A core older than O-10 does not send the field.
    client = IpcClient("keygnosys-test-unused")
    seen = []
    client.inertChanged.connect(seen.append)

    _mode(client, mode="cursor", latched=True, activation="hybrid")

    assert seen == [False]


def test_mode_event_drives_protection_changed(qtbot):
    client = IpcClient("keygnosys-test-unused")
    seen = []
    client.protectionChanged.connect(lambda s, r: seen.append((s, r)))

    _mode(client, mode="normal", latched=False, activation="hybrid",
          protection="unprotected", protection_reason="spawn_failed")
    _mode(client, mode="normal", latched=False, activation="hybrid",
          protection="protected", protection_reason="")

    assert seen == [("unprotected", "spawn_failed"), ("protected", "")]


def test_mode_event_without_protection_reads_as_not_applicable(qtbot):
    # A core older than the guardian does not send the fields.
    client = IpcClient("keygnosys-test-unused")
    seen = []
    client.protectionChanged.connect(lambda s, r: seen.append((s, r)))

    _mode(client, mode="cursor", latched=True, activation="hybrid")

    assert seen == [("not_applicable", "")]


def test_bindings_id_comes_from_hello_and_follows_config_changed(qtbot):
    client = IpcClient("keygnosys-test-unused")
    seen = []
    client.configChanged.connect(seen.append)
    assert client.bindings_id is None

    client._dispatch({"v": 1, "t": "event", "n": "hello",
                      "d": {"protocol": "1.0", "bindings_id": "default"}})
    assert client.bindings_id == "default"

    payload = {"kinds": ["bindings"], "bindings_id": "alt"}
    client._dispatch({"v": 1, "t": "event", "n": "config_changed",
                      "d": payload})

    assert client.bindings_id == "alt"
    assert seen == [payload]
