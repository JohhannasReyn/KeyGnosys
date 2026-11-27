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
