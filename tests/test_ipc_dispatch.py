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
