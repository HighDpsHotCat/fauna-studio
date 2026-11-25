from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPixmap
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QPushButton, QSizePolicy, QVBoxLayout


STYLE = """
QWidget { font-family: "Malgun Gothic", "Segoe UI", sans-serif; font-size: 13px; color: #243a31; }
QWidget#root, QScrollArea, QWidget#canvas { background: #f5f6f1; }
QDialog#lowpoly { background: #f5f6f1; }
QDialog#lowpoly QSpinBox { background: #fff; color: #243a31; border: 1px solid #a7b8a6; border-radius: 5px;
    padding: 5px 8px; selection-background-color: #395e45; selection-color: white; }
QDialog#lowpoly QSpinBox:focus { border-color: #527957; }
QScrollArea { border: none; }
QFrame#mast { background: #1d3b31; }
QLabel#wordmark { font: bold 25px "Georgia"; color: #f7f6e9; }
QLabel#strap { color: #d3dfce; font-size: 13px; }
QLabel#status { color: #e0eacf; font-size: 12px; }
QFrame#paper, QFrame#card { background: #ffffff; border: 1px solid #dce2d8; border-radius: 12px; }
QLabel#heading { font-size: 23px; font-weight: bold; color: #173c2d; }
QLabel#section { font-size: 16px; font-weight: bold; }
QLabel#hint, QLabel#credit { color: #52665b; font-size: 12px; }
QLabel#eyebrow { color: #56725f; font-size: 12px; font-weight: bold; }
QLabel#species { font: italic 23px "Georgia"; color: #334f41; }
QLabel#fileTitle { font-weight: bold; }
QLabel#empty { background: #eef2e9; border: 1px dashed #a5b59d; border-radius: 8px; color: #435c4a; }
QFrame#divider { background: #e1e6dc; max-height: 1px; }
QPushButton { background: #fff; border: 1px solid #cbd5c9; border-radius: 6px; padding: 8px 14px; font-weight: bold; }
QPushButton:hover { background: #f0f4eb; border-color: #829783; }
QPushButton:focus { border: 2px solid #54815e; }
QPushButton#primary { background: #28513b; color: white; border: 1px solid #28513b; }
QPushButton#primary:hover { background: #396949; }
QPushButton:disabled { background: #f3f4f0; color: #859189; border-color: #e1e5df; }
QPushButton#candidate { text-align: left; font: 14px "Segoe UI"; padding: 8px 11px; }
QPushButton#candidate:checked { background: #e9f1e4; border: 1px solid #688963; color: #1c462d; }
QPushButton#link { color: #36613e; border: none; padding: 4px 0; background: transparent; text-align: left; }
QPushButton#link:hover { text-decoration: underline; }
QProgressBar { background: #dce5d8; border: none; border-radius: 2px; max-height: 4px; }
QProgressBar::chunk { background: #729167; }
QScrollBar:vertical { width: 10px; background: transparent; }
QScrollBar::handle:vertical { background: #c2cebf; border: 2px solid #f5f6f1; border-radius: 4px; min-height: 30px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
"""


def label(text, style="", wrap=False):
    widget = QLabel(text)
    widget.setTextFormat(Qt.TextFormat.PlainText)
    widget.setObjectName(style)
    widget.setWordWrap(wrap)
    return widget


def button(text, handler, style=""):
    widget = QPushButton(text)
    widget.setObjectName(style)
    widget.setCursor(Qt.CursorShape.PointingHandCursor)
    widget.clicked.connect(handler)
    return widget


def panel(name="paper", margin=18):
    frame = QFrame()
    frame.setObjectName(name)
    layout = QVBoxLayout(frame)
    layout.setContentsMargins(margin, margin, margin, margin)
    layout.setSpacing(10)
    return frame, layout


def line(text, style="hint"):
    widget = QLabel()
    widget.setTextFormat(Qt.TextFormat.PlainText)
    widget.setObjectName(style)
    widget.setText(text)
    widget.setToolTip(text)
    widget.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
    return widget


class Picture(QFrame):
    def __init__(self, height, *, cover=True, placeholder=""):
        super().__init__()
        policy = QSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Preferred)
        if height is None:
            policy.setHeightForWidth(True)
        else:
            self.setFixedHeight(height)
        self.setSizePolicy(policy)
        self.cover = cover
        self.placeholder = placeholder
        self.pixmap = QPixmap()

    def heightForWidth(self, width):
        return min(465, width * 2 // 3)

    def set_picture(self, pixmap):
        self.pixmap = pixmap
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHints(QPainter.RenderHint.Antialiasing | QPainter.RenderHint.SmoothPixmapTransform)
        bounds = QRectF(self.rect())
        clip = QPainterPath()
        clip.addRoundedRect(bounds, 7, 7)
        painter.setClipPath(clip)
        painter.fillRect(bounds, QColor("#e9eee5"))
        if self.pixmap.isNull():
            painter.setPen(QColor("#586f59"))
            painter.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, self.placeholder)
            return
        width, height = self.pixmap.width(), self.pixmap.height()
        scale = (max if self.cover else min)(bounds.width() / width, bounds.height() / height)
        target = QRectF(0, 0, width * scale, height * scale)
        target.moveCenter(bounds.center())
        painter.drawPixmap(target, self.pixmap, QRectF(self.pixmap.rect()))


class PhotoCard(QFrame):
    def __init__(self, photo, open_page, generate):
        super().__init__()
        self.setObjectName("card")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 10, 10, 9)
        layout.setSpacing(4)
        self.picture = Picture(133, placeholder="사진 불러오는 중…")
        layout.addWidget(self.picture)
        layout.addWidget(line(photo.title, "fileTitle"))
        layout.addWidget(line(photo.author))
        layout.addWidget(line(photo.license, "credit"))
        bottom = QHBoxLayout()
        bottom.addWidget(button("원문 열기 ↗", open_page, "link"))
        bottom.addStretch()
        bottom.addWidget(label(photo.basis, "hint"))
        layout.addLayout(bottom)
        self.generate = button("이 사진으로 3D 생성…", generate)
        layout.addWidget(self.generate)
