#include "StyleSheet.h"

QString styleSheetFor(const Theme &t)
{
    QString css = QStringLiteral(R"(
QMainWindow, QWidget#chatPane, QListView#messageList { background: @window; color: @fg; }
QWidget#sidebar { background: @sidebar; border-right: 1px solid @border; }
QLabel#sidebarTitle { color: @fg; font-weight: bold; font-size: 15px; padding: 10px 12px 4px 12px; }
QLabel#connectionState { color: @dimFg; font-size: 12px; padding: 0 12px 4px 12px; }
QLabel#notice { color: @fg; background: @pathBg; border-radius: 8px; padding: 6px 8px; margin: 2px 8px 4px 8px; }
QToolButton#attachButton { background: transparent; border: none; border-radius: 18px; padding: 6px; font-size: 18px; }
QToolButton#attachButton:hover { background: @hover; }
QToolButton#safetyNumberButton { color: @fg; background: @pathBg; border: none; border-radius: 8px; padding: 5px 10px; }
QToolButton#safetyNumberButton:hover { background: @hover; }
QLineEdit#searchField { background: @pathBg; border: none; border-radius: 8px; padding: 6px 8px; margin: 4px 8px 8px 8px; color: @fg; }
QListWidget#conversationList { background: @sidebar; color: @fg; border: none; outline: 0; padding: 0 6px; }
QWidget#chatHeader { background: @window; border-bottom: 1px solid @border; }
QLabel#chatTitle { color: @fg; font-weight: bold; font-size: 15px; }
QLabel#chatSubtitle { color: @dimFg; }
QListView#messageList { border: none; outline: 0; }
QWidget#requestBar { background: @window; border-top: 1px solid @border; }
QLabel#leftNote { color: @dimFg; background: @window; border-top: 1px solid @border; padding: 16px; }
QLabel#requestText { color: @fg; }
QWidget#requestBar QPushButton { background: @pathBg; color: @fg; border: none; border-radius: 16px; padding: 7px 18px; }
QWidget#requestBar QPushButton:hover { background: @hover; }
QWidget#requestBar QPushButton#acceptButton { background: @accent; color: #ffffff; font-weight: bold; }
QWidget#composer { background: @window; border-top: 1px solid @border; }
QWidget#replyBar { background: @pathBg; border-left: 3px solid @accent; border-radius: 6px; }
QLabel#replyLabel { color: @dimFg; }
QWidget#recordBar { background: @pathBg; border-radius: 6px; }
QWidget#attachBar { background: transparent; }
QWidget#attachChip { background: @pathBg; border: 1px solid @border; border-radius: 8px; }
QLabel#recordLabel { color: #e01b24; font-weight: bold; }
QWidget#findBar { background: @window; border-bottom: 1px solid @border; }
QLineEdit#findField { background: @pathBg; border: none; border-radius: 8px; padding: 5px 8px; color: @fg; }
QLabel#findCount { color: @dimFg; padding: 0 6px; }
QPlainTextEdit#composeField { background: @pathBg; color: @fg; border: none; border-radius: 18px; padding: 6px 12px;
    selection-background-color: @accent; }
QPushButton#sendButton { background: @accent; color: #ffffff; border: none; border-radius: 18px; padding: 8px 16px; font-weight: bold; }
QPushButton#sendButton:disabled { background: @pathBg; color: @dimFg; }
QLabel#emptyNote { color: @dimFg; }
QSplitter::handle { background: @border; }
QSplitter::handle:horizontal { width: 1px; }
QStatusBar { background: @sidebar; color: @dimFg; }
QMenu { background: @popup; color: @fg; border: 1px solid @border; padding: 4px; }
QMenu::item { padding: 5px 20px; border-radius: 4px; }
QMenu::item:selected { background: @hover; }
QMenu::item:disabled { color: @dimFg; }
QMenuBar { background: @sidebar; color: @fg; }
QMenuBar::item:selected { background: @hover; }
QScrollBar:vertical { background: transparent; width: 10px; }
QScrollBar::handle:vertical { background: @scroll; border-radius: 4px; min-height: 30px; margin: 2px; }
QScrollBar::handle:vertical:hover { background: @scrollHover; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }
)");
    const QList<QPair<QString, QColor>> tokens{
        {QStringLiteral("@window"), t.window},   {QStringLiteral("@sidebar"), t.sidebar},
        {QStringLiteral("@fg"), t.fg},           {QStringLiteral("@dimFg"), t.dimFg},
        {QStringLiteral("@hover"), t.hover},     {QStringLiteral("@pressed"), t.pressed},
        {QStringLiteral("@selected"), t.selected}, {QStringLiteral("@itemSelected"), t.itemSelected},
        {QStringLiteral("@pathBg"), t.pathBg},   {QStringLiteral("@popup"), t.popup},
        {QStringLiteral("@border"), t.border},   {QStringLiteral("@scrollHover"), t.scrollHover},
        {QStringLiteral("@scroll"), t.scroll},   {QStringLiteral("@accent"), t.accent},
    };
    // Longer names first: @scrollHover before @scroll, @itemSelected before @selected.
    for (const auto &token : tokens)
        css.replace(token.first, cssColor(token.second));
    return css;
}
