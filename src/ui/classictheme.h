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

#ifndef CLASSICTHEME_H
#define CLASSICTHEME_H

#include <QString>
#include <QtGlobal>

class QApplication;
class QFont;
class QMenu;
class QWidget;

//! Flat "classic" light / dark application theme (palette + stylesheet).
//! The choice is stored in the settings; switching restarts the app so every
//! widget and graph picks up the new colours consistently.
namespace ClassicTheme
{
    //! Reads the saved choice and applies Fusion, palette and stylesheet.
    //! Call right after the QApplication is created (before CFG->Load()).
    void Apply(QApplication &app);
    bool IsDark();
    //! Saves the new choice and relaunches the application.
    void SwitchAndRestart(bool dark);
    //! Adds a checkable "Dark mode" action to a context menu.
    void AddToggleAction(QMenu *menu);
    //! Snaps font size to integer physical screen pixels without antialiasing.
    QFont SnapCrispFont(const QString &family = QStringLiteral("Arial"), qreal desiredPt = 10.0);
}

#endif // CLASSICTHEME_H
