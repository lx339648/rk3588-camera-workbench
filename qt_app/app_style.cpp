#include "app_style.hpp"

QString appStyleSheet() {
    return QString::fromUtf8(R"QSS(
        QWidget {
            color: #242424;
            background-color: #ffffff;
        }
        QWidget#windowRoot, QWidget#mainContent, QWidget#contentPage {
            background-color: #ffffff;
        }
        QFrame#sidebar {
            background-color: #f7f7f5;
            border-right: 1px solid #e8e8e6;
        }
        QLabel#appName {
            color: #202123;
            font-size: 20px;
            font-weight: 600;
        }
        QLabel#pageTitle {
            color: #202123;
            font-size: 24px;
            font-weight: 600;
        }
        QLabel#sectionTitle {
            color: #202123;
            font-size: 17px;
            font-weight: 600;
        }
        QLabel#mutedText {
            color: #777b80;
            font-size: 13px;
        }
        QLabel#eyebrow {
            color: #85898d;
            font-size: 12px;
            font-weight: 600;
        }
        QLabel#videoHint {
            color: #d9dee3;
            background: transparent;
            font-size: 18px;
        }
        QLabel#fpsLabel {
            color: #6c7378;
            background: transparent;
            font-size: 12px;
        }
        QLabel#powerStatus {
            color: #777b80;
            background: transparent;
            font-size: 12px;
            padding: 0 8px 4px 8px;
        }
        QFrame#card {
            background-color: #ffffff;
            border: 1px solid #e9e9e7;
            border-radius: 16px;
        }
        QFrame#videoSurface {
            background-color: #171a1e;
            border: none;
            border-radius: 12px;
        }
        QPushButton#navButton {
            text-align: left;
            background-color: transparent;
            border: none;
            border-radius: 10px;
            padding: 11px 14px;
            color: #43474b;
        }
        QPushButton#navButton:hover {
            background-color: #eeeeeb;
        }
        QPushButton#navButton:checked {
            background-color: #e9e9e6;
            color: #202123;
            font-weight: 600;
        }
        QPushButton#powerButton {
            min-height: 42px;
            text-align: left;
            padding: 4px 14px;
            color: #303338;
            background-color: #ffffff;
            border: 1px solid #dcdedc;
            border-radius: 10px;
            font-weight: 600;
        }
        QPushButton#powerButton:hover {
            background-color: #f0f2ef;
        }
        QPushButton#powerButton:checked {
            color: #087657;
            background-color: #e4f4ee;
            border-color: #b8e0d1;
        }
        QPushButton#primaryButton, QPushButton#secondaryButton {
            min-height: 38px;
            border-radius: 10px;
            padding: 5px 14px;
            font-weight: 600;
        }
        QPushButton#primaryButton {
            color: #ffffff;
            background-color: #10a37f;
            border: 1px solid #10a37f;
        }
        QPushButton#primaryButton:hover {
            background-color: #0c8f70;
            border-color: #0c8f70;
        }
        QPushButton#primaryButton:checked {
            color: #087657;
            background-color: #e4f4ee;
            border-color: #b8e0d1;
        }
        QPushButton#primaryButton:disabled {
            color: #a5a8aa;
            background-color: #f5f5f4;
            border-color: #e9e9e7;
        }
        QPushButton#secondaryButton {
            color: #303338;
            background-color: #ffffff;
            border: 1px solid #dcdedc;
        }
        QPushButton#secondaryButton:hover {
            background-color: #f6f7f6;
        }
        QPushButton#secondaryButton:checked {
            color: #087657;
            background-color: #edf7f2;
            border-color: #b8e0d1;
        }
        QPushButton#secondaryButton:disabled {
            color: #a5a8aa;
            background-color: #f5f5f4;
            border-color: #e9e9e7;
        }
        QLineEdit {
            min-height: 38px;
            padding: 2px 12px;
            background-color: #ffffff;
            border: 1px solid #dcdedc;
            border-radius: 10px;
            selection-background-color: #bce7d6;
        }
        QLineEdit:focus {
            border: 1px solid #10a37f;
        }
        QListWidget {
            background-color: #fbfbfa;
            border: 1px solid #e9e9e7;
            border-radius: 10px;
            outline: none;
        }
        QListWidget::item {
            padding: 9px;
        }
        QListWidget::item:selected {
            color: #087657;
            background-color: #e4f4ee;
        }
        QToolButton#deleteFileButton {
            color: #a03d3d;
            background-color: transparent;
            border: none;
            border-radius: 6px;
            padding: 4px 7px;
        }
        QToolButton#deleteFileButton:hover {
            background-color: #fbe9e9;
        }
        QSlider::groove:horizontal {
            height: 5px;
            background: #e5e7e6;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            width: 12px;
            margin: -4px 0;
            border-radius: 6px;
            background: #10a37f;
        }
    )QSS");
}
