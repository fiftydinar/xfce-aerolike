import libcalamares
from PyQt6.QtWidgets import QWidget, QVBoxLayout, QRadioButton, QButtonGroup, QLabel, QSpacerItem, QSizePolicy

_widget = None
_offline = None
_online = None


def createWidget():
    global _widget, _offline, _online

    _widget = QWidget()
    layout = QVBoxLayout()
    layout.setContentsMargins(20, 20, 20, 20)
    layout.setSpacing(12)

    title = QLabel("<b>Select install mode</b>")
    title.setStyleSheet("font-size: 14px;")
    layout.addWidget(title)

    desc = QLabel(
        "Choose how the system image is sourced for installation:"
    )
    desc.setWordWrap(True)
    layout.addWidget(desc)

    _offline = QRadioButton(
        "Offline - use image bundled in the ISO (no network required)"
    )
    _offline.setChecked(True)
    layout.addWidget(_offline)

    _online = QRadioButton(
        "Online - pull the latest image from the registry"
    )
    layout.addWidget(_online)

    group = QButtonGroup()
    group.addButton(_offline)
    group.addButton(_online)

    layout.addSpacerItem(
        QSpacerItem(20, 20, QSizePolicy.Policy.Minimum, QSizePolicy.Policy.Expanding)
    )

    _widget.setLayout(layout)
    return _widget


def onActivate():
    pass


def onLeave():
    global _offline, _online
    mode = "offline" if _offline.isChecked() else "online"
    libcalamares.globalstorage.insert("installMode", mode)
    try:
        with open("/opt/install/install-mode", "w") as f:
            f.write(mode + "\n")
    except OSError as e:
        libcalamares.utils.debug(f"failed to write install-mode: {e}")
    libcalamares.utils.debug(f"installMode set to {mode}")


def isNextEnabled():
    return True


def prettyName():
    return "Install Mode"


def retranslate():
    pass
