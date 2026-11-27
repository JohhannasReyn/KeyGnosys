"""How the overlay decides, at startup, whether the native core is running."""
from __future__ import annotations

from keygnosys.app import _core_present


def test_windows_finds_a_listening_core_in_the_pipe_namespace() -> None:
    # os.path.exists() is always False for a named pipe, which sent `auto` to
    # the mock even with the core running. The pipe listing is the check.
    def listdir(path: str) -> list[str]:
        assert path == "\\\\.\\pipe\\"
        return ["other", "keygnosys"]
    assert _core_present("\\\\.\\pipe\\keygnosys", "win32", listdir) is True


def test_windows_without_the_pipe_is_not_present() -> None:
    assert _core_present("\\\\.\\pipe\\keygnosys", "win32",
                         lambda _p: ["other"]) is False


def test_windows_with_an_unreadable_pipe_namespace_is_not_present() -> None:
    def listdir(_p: str) -> list[str]:
        raise OSError("denied")
    assert _core_present("\\\\.\\pipe\\keygnosys", "win32", listdir) is False


def test_other_platforms_use_the_socket_path(tmp_path) -> None:
    sock = tmp_path / "keygnosys.sock"
    assert _core_present(str(sock), "linux") is False
    sock.write_text("")
    assert _core_present(str(sock), "linux") is True
