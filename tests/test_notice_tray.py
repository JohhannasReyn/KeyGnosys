"""The blocked-action notice tray: a strip that slides out under the overlay."""

from PySide6.QtCore import QRect

from keygnosys.ui.notice_tray import NoticeTray


def test_showing_a_notice_docks_it_centred_under_the_keyboard(qtbot):
    tray = NoticeTray()
    qtbot.addWidget(tray)
    tray.show_notice("Drag lock refused", QRect(100, 200, 1000, 300), duration_s=4.0)

    assert tray.isVisible()
    assert tray.text() == "Drag lock refused"
    geo = tray.geometry()
    assert geo.top() >= 200 + 300           # below the keyboard, not over it
    assert abs(geo.center().x() - (100 + 1000 // 2)) <= 1


def test_the_notice_hides_itself_after_its_duration(qtbot):
    tray = NoticeTray()
    qtbot.addWidget(tray)
    tray.show_notice("Drag lock refused", QRect(0, 0, 400, 100), duration_s=0.05)
    qtbot.waitUntil(lambda: not tray.isVisible(), timeout=2000)


def test_a_second_notice_restarts_the_timer(qtbot):
    tray = NoticeTray()
    qtbot.addWidget(tray)
    tray.show_notice("first", QRect(0, 0, 400, 100), duration_s=0.3)
    qtbot.wait(200)
    tray.show_notice("second", QRect(0, 0, 400, 100), duration_s=0.3)
    qtbot.wait(200)
    assert tray.isVisible() and tray.text() == "second"
    qtbot.waitUntil(lambda: not tray.isVisible(), timeout=2000)


def test_redocking_follows_the_keyboard(qtbot):
    tray = NoticeTray()
    qtbot.addWidget(tray)
    tray.show_notice("x", QRect(0, 0, 400, 100), duration_s=4.0)
    tray.dock_under(QRect(500, 600, 400, 100))
    assert tray.geometry().top() >= 700
    assert abs(tray.geometry().center().x() - 700) <= 1


def test_notices_are_coloured_by_level(qtbot):
    from keygnosys.ui.notice_tray import LEVEL_COLOURS
    tray = NoticeTray()
    qtbot.addWidget(tray)
    for level in ("success", "warning", "error"):
        tray.show_notice("x", QRect(0, 0, 400, 100), duration_s=1.0, level=level)
        assert tray.level() == level
        assert tray.background() == LEVEL_COLOURS[level][0]
    assert LEVEL_COLOURS["success"][0].green() > LEVEL_COLOURS["success"][0].red()
    yellow = LEVEL_COLOURS["warning"][0]
    assert yellow.red() > 200 and yellow.green() > 180 and yellow.blue() < 120
    assert LEVEL_COLOURS["error"][0].red() > LEVEL_COLOURS["error"][0].green()


def test_a_notice_is_an_error_unless_told_otherwise(qtbot):
    tray = NoticeTray()
    qtbot.addWidget(tray)
    tray.show_notice("Drag lock refused", QRect(0, 0, 400, 100), duration_s=1.0)
    assert tray.level() == "error"


def test_yellow_notices_use_dark_text(qtbot):
    tray = NoticeTray()
    qtbot.addWidget(tray)
    tray.show_notice("x", QRect(0, 0, 400, 100), duration_s=1.0, level="warning")
    assert "#1a1a1a" in tray._label.styleSheet()
    tray.show_notice("x", QRect(0, 0, 400, 100), duration_s=1.0, level="success")
    assert "white" in tray._label.styleSheet()
