/*
 * Tux Manager - Linux system monitor
 * Copyright (C) 2026 Petr Bena <petr@bena.rocks>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "mainwindow.h"
#include "configuration.h"
#include "i18n.h"
#include "logger.h"
#include "globals.h"
#include "ui/classictheme.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QIcon>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>
#include <QSocketNotifier>
#include "os/selfpriority.h"
#include <cstdio>
#include <cstring>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

// Pre-screen --help / --version before QApplication so they work without a display.
static void print_version()
{
    printf("%s %s\n", TUX_MANAGER_PRODUCT_NAME, TUX_MANAGER_VERSION_STRING);
}

// ── Single instance ───────────────────────────────────────────────────────────
// Launching Tux Manager while it is already running toggles it: the new process
// asks the running one to close normally (so it saves its settings), kills it if
// it does not exit in time, and then exits without opening a window. So the
// hotkey opens, closes, opens... With --replace (used by the dark-mode restart)
// the new process opens its own window after the old one is gone.

static QString instanceServerName()
{
    // TUX_MANAGER_INSTANCE lets a second, independent copy run (used for testing).
    const QString suffix = qEnvironmentVariable("TUX_MANAGER_INSTANCE");
    return QString("tux-manager-%1%2").arg(::getuid()).arg(suffix.isEmpty() ? QString() : "-" + suffix);
}

enum class OldInstance { None, Closed, Killed };

//! Closes a running instance: None = nobody was running, Closed = it closed
//! normally, Killed = it was frozen and had to be killed.
static OldInstance closeRunningInstance()
{
    QLocalSocket socket;
    socket.connectToServer(instanceServerName());
    if (!socket.waitForConnected(500))
        return OldInstance::None; // nobody running

    // PID of the running instance from the socket's peer credentials; this works
    // even when that instance is frozen and can't answer.
    qint64 oldPid = 0;
    struct ucred cred{};
    socklen_t credLen = sizeof(cred);
    if (::getsockopt(static_cast<int>(socket.socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &cred, &credLen) == 0)
        oldPid = cred.pid;

    // Ask it to close normally; it answers with its PID before closing.
    socket.write("quit\n");
    socket.flush();
    // A healthy instance answers within milliseconds. No answer = its window is
    // frozen; do not make the user wait long for a task manager.
    const bool answered = socket.waitForReadyRead(1500);
    if (answered && oldPid <= 0)
        oldPid = socket.readLine().trimmed().toLongLong();

    QElapsedTimer timer;
    timer.start();
    const int patience = answered ? 3000 : 0;
    while (socket.state() != QLocalSocket::UnconnectedState && timer.elapsed() < patience)
        socket.waitForDisconnected(100);

    // Wait until the process is really gone (its settings are written on close).
    while (oldPid > 0 && ::kill(static_cast<pid_t>(oldPid), 0) == 0 && timer.elapsed() < patience)
        QThread::msleep(50);

    if (oldPid > 0 && ::kill(static_cast<pid_t>(oldPid), 0) == 0)
    {
        LOG_INFO(QString("Previous instance %1 did not exit, killing it").arg(oldPid));
        ::kill(static_cast<pid_t>(oldPid), SIGKILL);
        for (int i = 0; i < 40 && ::kill(static_cast<pid_t>(oldPid), 0) == 0; ++i)
            QThread::msleep(25);
        return OldInstance::Killed;
    }
    return OldInstance::Closed;
}

// ── Signals ───────────────────────────────────────────────────────────────────
// SIGTERM / SIGINT / SIGHUP (logout, kill, Ctrl+C) close the window normally so
// settings are saved. The handler only writes one byte to a socket pair; the
// actual work happens in the event loop.
static int g_signalFds[2] = { -1, -1 };

static void onUnixSignal(int)
{
    const char byte = 1;
    const ssize_t ignored = ::write(g_signalFds[0], &byte, 1);
    (void) ignored;
}

static void installSignalHandlers(QWidget *window)
{
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, g_signalFds) != 0)
        return;
    QSocketNotifier *notifier = new QSocketNotifier(g_signalFds[1], QSocketNotifier::Read, window);
    QObject::connect(notifier, &QSocketNotifier::activated, window, [window, notifier]()
    {
        notifier->setEnabled(false);
        window->close();
        QCoreApplication::quit();
    });
    struct sigaction action{};
    action.sa_handler = onUnixSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    for (int sig : { SIGTERM, SIGINT, SIGHUP })
        ::sigaction(sig, &action, nullptr);
    // Writing to a closed pipe / socket must never kill a task manager.
    ::signal(SIGPIPE, SIG_IGN);
}

static void listenForNewInstances(QLocalServer &server, QWidget *window)
{
    QLocalServer::removeServer(instanceServerName()); // stale socket from a crash
    if (!server.listen(instanceServerName()))
        return;

    QObject::connect(&server, &QLocalServer::newConnection, &server, [&server, window]()
    {
        while (QLocalSocket *client = server.nextPendingConnection())
        {
            client->write(QByteArray::number(static_cast<qint64>(::getpid())) + "\n");
            client->flush();
            client->waitForBytesWritten(500);
            // Close the window normally (saves geometry/settings), then exit.
            window->close();
            QCoreApplication::quit();
        }
    });
}

static void print_help()
{
    printf("Usage: %s [options]\n\n"
           "Linux system monitor inspired by Windows Task Manager.\n\n"
           "Options:\n"
           "  -h, --help     Show this help and exit\n"
           "  -V, --version  Show version and exit\n"
           "      --force-drm Force DRM GPU backend and skip NVML detection\n"
           "  -v             Increase log verbosity.\n"
           "                 Repeat for more detail: -v, -vv, -vvv\n",
           TUX_MANAGER_PRODUCT_NAME);
}

int main(int argc, char *argv[])
{
    // Handle --help / --version before constructing QApplication so they
    // are usable from a terminal without a display server present.
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-?") == 0)
        {
            print_help();
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0)
        {
            print_version();
            return 0;
        }
    }

    // Enable classic TrueType interpreter v35 for crisp hard-pixel rendering without antialiasing.
    if (!qEnvironmentVariableIsSet("FREETYPE_PROPERTIES"))
    {
        qputenv("FREETYPE_PROPERTIES", "truetype:interpreter-version=35");
    }

    // Qt's raster painter splits large fills (every graph background at 4K) across a
    // thread pool; waking those threads on every repaint cost ~30% of the Performance
    // tab's CPU, more than the fills themselves. Paint on the GUI thread only.
    if (!qEnvironmentVariableIsSet("QT_NO_GUI_THREADPOOL"))
        qputenv("QT_NO_GUI_THREADPOOL", "1");

    // Render the UI at 0.9 (90%) size by default.
    // Override with e.g. TUX_MANAGER_SCALE=1.0 or QT_SCALE_FACTOR without rebuilding.
    if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR"))
    {
        const QByteArray scale = qEnvironmentVariableIsSet("TUX_MANAGER_SCALE")
                                 ? qgetenv("TUX_MANAGER_SCALE") : QByteArray("0.9");
        qputenv("QT_SCALE_FACTOR", scale);
    }

    QApplication a(argc, argv);
    a.setOrganizationName(TUX_MANAGER_PRODUCT_NAME);
    a.setApplicationName(TUX_MANAGER_PRODUCT_NAME);
    a.setApplicationVersion(TUX_MANAGER_VERSION_STRING);
    a.setWindowIcon(QIcon(":/tux_manager_256.ico"));

    // ── Classic flat look, light or dark (saved choice) ──────────────────────
    ClassicTheme::Apply(a);

    // ── Command-line parsing (remaining options) ──────────────────────────────
    QCommandLineParser parser;
    parser.setApplicationDescription("Linux system monitor inspired by Windows Task Manager.");
    parser.addHelpOption();

    QCommandLineOption versionOption(QStringList{"V", "version"}, "Display version information and exit.");
    parser.addOption(versionOption);

    QCommandLineOption verboseOption(QStringList{"v"}, "Increase verbosity. Repeat for more detail: -v, -vv, -vvv.");
    parser.addOption(verboseOption);
    QCommandLineOption forceDrmOption(QStringList{"force-drm"}, "Force DRM GPU backend and skip NVML detection.");
    parser.addOption(forceDrmOption);
    QCommandLineOption replaceOption(QStringList{"replace"}, "If Tux Manager is already running, close it and open a new window (default: just close it).");
    parser.addOption(replaceOption);

    parser.process(a);

    // Count -v occurrences manually: handles -v, -vv, -vvv and mixed forms.
    // QCommandLineParser doesn't expose a count() in all Qt versions.
    int verbosity = 0;
    for (const QString &arg : a.arguments().mid(1))
    {
        if (arg.startsWith("--")) continue;
        if (arg.startsWith('-'))
        {
            for (const QChar c : arg.mid(1))
                if (c == 'v') verbosity++;
        }
    }
    Logger::instance()->Verbosity = verbosity;

    // ── Single instance: launching again closes the running window (toggle) ──
    // A frozen old instance was not something the user could see or use, so in that
    // case open a fresh window instead of only removing it.
    const OldInstance old = closeRunningInstance();
    if (old == OldInstance::Closed && !parser.isSet(replaceOption))
        return 0;

    // ── Bootstrap ─────────────────────────────────────────────────────────────
    CFG->Load();
    CFG->ForceGpuDrm = parser.isSet(forceDrmOption);

    // Install translations for the system language before any widget is
    // created, so all tr() calls already resolve to the right text.
    I18n::InstallTranslators();

    LOG_INFO(QString("%1 %2 starting (verbosity=%3)").arg(a.applicationName(), a.applicationVersion()).arg(verbosity));

    MainWindow w;
    QLocalServer instanceServer;
    listenForNewInstances(instanceServer, &w);
    installSignalHandlers(&w);
    w.show();
    OS::BoostOwnPriority();
    return a.exec();
}
