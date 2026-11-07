/*
 * Tux Manager - Linux system monitor
 * Copyright (C) 2026 Petr Bena <petr@bena.rocks>
 * Copyright (C) 2026 Tosskek (classic look)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "classictheme.h"

#include <QAction>
#include <QApplication>
#include <QFont>
#include <QMenu>
#include <QPalette>
#include <QProcess>
#include <QSettings>
#include <QStyleFactory>
#include <QTimer>
#include <QWidget>

namespace
{
    bool g_dark = false;
    const char *kSettingKey = "General/ClassicDarkMode";

    struct Tokens
    {
        const char *window, *text, *base, *altBase, *muted;
        const char *menuBg, *menuBorder, *menuSel, *menuSelText;
        const char *tabBg, *tabBorder, *hover;
        const char *selection, *selectionText;
        const char *btnBg, *btnBorder, *btnHoverBg, *btnHoverBorder, *btnPressedBg, *btnDisabledBg, *btnDisabledText;
        const char *editBorder, *accent;
    };

    const Tokens kLight = {
        "#ffffff", "#000000", "#ffffff", "#f7f7f7", "#808080",
        "#f2f2f2", "#cccccc", "#91c9f7", "#000000",
        "#f0f0f0", "#d9d9d9", "#e5f3ff",
        "#cce8ff", "#000000",
        "#e1e1e1", "#adadad", "#e5f1fb", "#0078d7", "#cce4f7", "#cccccc", "#838383",
        "#7a7a7a", "#0078d7"
    };

    const Tokens kDark = {
        "#202020", "#e8e8e8", "#1a1a1a", "#242424", "#8c8c8c",
        "#2b2b2b", "#454545", "#3d5f80", "#ffffff",
        "#2b2b2b", "#3c3c3c", "#2a3b4d",
        "#264f78", "#ffffff",
        "#333333", "#5a5a5a", "#3a4654", "#3a96dd", "#24527d", "#2a2a2a", "#707070",
        "#6a6a6a", "#3a96dd"
    };
}

bool ClassicTheme::IsDark()
{
    return g_dark;
}

void ClassicTheme::Apply(QApplication &app)
{
    g_dark = QSettings().value(kSettingKey, false).toBool();
    const Tokens &t = g_dark ? kDark : kLight;

    app.setStyle(QStyleFactory::create("Fusion"));
    {
        // Arial everywhere (painted text included).
        QFont font("Arial", app.font().pointSize() > 0 ? app.font().pointSize() : 10);
        app.setFont(font);
    }

    QPalette pal;
    pal.setColor(QPalette::Window, QColor(t.window));
    pal.setColor(QPalette::WindowText, QColor(t.text));
    pal.setColor(QPalette::Base, QColor(t.base));
    pal.setColor(QPalette::AlternateBase, QColor(t.altBase));
    pal.setColor(QPalette::Text, QColor(t.text));
    pal.setColor(QPalette::Button, QColor(t.btnBg));
    pal.setColor(QPalette::ButtonText, QColor(t.text));
    pal.setColor(QPalette::ToolTipBase, QColor(t.menuBg));
    pal.setColor(QPalette::ToolTipText, QColor(t.text));
    pal.setColor(QPalette::PlaceholderText, QColor(t.muted));
    pal.setColor(QPalette::Highlight, QColor(t.selection));
    pal.setColor(QPalette::HighlightedText, QColor(t.selectionText));
    pal.setColor(QPalette::Link, QColor(t.accent));
    pal.setColor(QPalette::Mid, QColor(t.tabBorder));
    pal.setColor(QPalette::Light, QColor(g_dark ? "#3a3a3a" : "#ffffff"));
    pal.setColor(QPalette::Dark, QColor(g_dark ? "#151515" : "#a0a0a0"));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(t.btnDisabledText));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(t.btnDisabledText));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(t.btnDisabledText));
    app.setPalette(pal);

    QString qss = QString::fromLatin1(R"(
        QMenuBar { background: @window; border: none; }
        QMenuBar::item { padding: 3px 7px; background: transparent; }
        QMenuBar::item:selected { background: @hover; }
        QMenu { background: @menuBg; border: 1px solid @menuBorder; padding: 2px; color: @text; }
        QMenu::item { padding: 4px 28px 4px 24px; }
        QMenu::item:selected { background: @menuSel; color: @menuSelText; }
        QTabWidget::pane { border: none; border-top: 1px solid @tabBorder; top: -1px; background: @window; }
        QTabBar::tab { background: @tabBg; border: 1px solid @tabBorder; border-bottom: none;
                       padding: 5px 12px; margin-right: -1px; color: @text; }
        QTabBar::tab:selected { background: @window; margin-top: -2px; padding-bottom: 7px; }
        QTabBar::tab:!selected:hover { background: @hover; }
        QHeaderView { background: @base; }
        QHeaderView::section { background: @base; color: @muted; border: none;
                               border-right: 1px solid @tabBorder; border-bottom: 1px solid @tabBorder;
                               padding: 4px 6px; }
        QHeaderView::section:hover { background: @hover; }
        QTreeView, QTableView { border: none; gridline-color: @tabBorder; background: @base;
                                selection-background-color: @selection; selection-color: @selectionText; }
        QTreeView::item:hover, QTableView::item:hover { background: @hover; }
        QPushButton { background: @btnBg; color: @text; border: 1px solid @btnBorder; padding: 4px 16px; min-height: 18px; }
        QPushButton:hover { background: @btnHoverBg; border-color: @btnHoverBorder; }
        QPushButton:pressed { background: @btnPressedBg; border-color: @btnHoverBorder; }
        QPushButton:disabled { background: @btnDisabledBg; border-color: @tabBorder; color: @btnDisabledText; }
        QToolButton#themeToggle { border: none; padding: 2px 8px; color: @text; background: transparent; }
        QToolButton#themeToggle:hover { background: @hover; }
        QLineEdit { border: 1px solid @editBorder; padding: 2px 4px; background: @base; color: @text; }
        QLineEdit:focus { border-color: @accent; }
        QSplitter::handle { background: @window; }
        QStatusBar { background: @window; }
        QToolTip { background: @menuBg; color: @text; border: 1px solid @menuBorder; }
    )");
    const QList<QPair<const char *, const char *>> map = {
        { "@window", t.window }, { "@text", t.text }, { "@base", t.base },
        { "@muted", t.muted }, { "@menuBg", t.menuBg }, { "@menuBorder", t.menuBorder },
        { "@menuSelText", t.menuSelText }, { "@menuSel", t.menuSel }, { "@tabBg", t.tabBg },
        { "@tabBorder", t.tabBorder }, { "@hover", t.hover }, { "@selectionText", t.selectionText },
        { "@selection", t.selection }, { "@btnHoverBorder", t.btnHoverBorder }, { "@btnHoverBg", t.btnHoverBg },
        { "@btnPressedBg", t.btnPressedBg }, { "@btnDisabledBg", t.btnDisabledBg },
        { "@btnDisabledText", t.btnDisabledText }, { "@btnBorder", t.btnBorder }, { "@btnBg", t.btnBg },
        { "@editBorder", t.editBorder }, { "@accent", t.accent }
    };
    for (const auto &kv : map)   // longer names come first where one is a prefix of another
        qss.replace(QLatin1String(kv.first), QLatin1String(kv.second));
    app.setStyleSheet(qss);
}

void ClassicTheme::SwitchAndRestart(bool dark)
{
    if (dark == g_dark)
        return;
    {
        QSettings s;
        s.setValue(kSettingKey, dark);
        s.sync();
    }
    // Close windows normally (they persist geometry/settings), then relaunch.
    QTimer::singleShot(0, qApp, []()
    {
        const QStringList args = QCoreApplication::arguments().mid(1);
        QProcess::startDetached(QCoreApplication::applicationFilePath(), args);
        const QList<QWidget *> windows = QApplication::topLevelWidgets();
        for (QWidget *w : windows)
        {
            if (w->isWindow() && w->isVisible())
                w->close();
        }
        QCoreApplication::quit();
    });
}

void ClassicTheme::AddToggleAction(QMenu *menu)
{
    if (!menu)
        return;
    QAction *action = menu->addAction(QObject::tr("Dark mode"));
    action->setCheckable(true);
    action->setChecked(g_dark);
    QObject::connect(action, &QAction::toggled, menu, [](bool on) { ClassicTheme::SwitchAndRestart(on); });
}
